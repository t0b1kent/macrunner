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
        default: return HB_SIZE_32;
    }
}

static hb_reg_t map_x86_reg_to_ir(int reg) {
    if (reg >= HB_REG_XMM0 && reg <= HB_REG_XMM7) return (hb_reg_t)reg;
    switch (reg) {
        case HB_REG_X86_EAX: return HB_REG_RAX;
        case HB_REG_X86_ECX: return HB_REG_RCX;
        case HB_REG_X86_EDX: return HB_REG_RDX;
        case HB_REG_X86_EBX: return HB_REG_RBX;
        case HB_REG_X86_ESP: return HB_REG_RSP;
        case HB_REG_X86_EBP: return HB_REG_RBP;
        case HB_REG_X86_ESI: return HB_REG_RSI;
        case HB_REG_X86_EDI: return HB_REG_RDI;
        case HB_REG_X86_EIP: return HB_REG_RIP;
        case HB_REG_X86_XMM0: return HB_REG_XMM0;
        case HB_REG_X86_XMM1: return HB_REG_XMM1;
        case HB_REG_X86_XMM2: return HB_REG_XMM2;
        case HB_REG_X86_XMM3: return HB_REG_XMM3;
        case HB_REG_X86_XMM4: return HB_REG_XMM4;
        case HB_REG_X86_XMM5: return HB_REG_XMM5;
        case HB_REG_X86_XMM6: return HB_REG_XMM6;
        case HB_REG_X86_XMM7: return HB_REG_XMM7;
        default: return HB_REG_COUNT;
    }
}

static hb_ir_operand_t operand_from_dec(const hb_decoded_t* dec, int slot) {
    const hb_decoded_t* d = dec;
    bool is_reg = (slot == 1) ? d->op1.is_reg : (slot == 2) ? d->op2.is_reg : d->op3.is_reg;
    bool is_imm = (slot == 1) ? d->op1.is_imm : (slot == 2) ? d->op2.is_imm : d->op3.is_imm;
    bool is_mem = (slot == 1) ? d->op1.is_mem : (slot == 2) ? d->op2.is_mem : d->op3.is_mem;
    uint8_t sz_raw = (slot == 1) ? d->op1.size : (slot == 2) ? d->op2.size : d->op3.size;
    uint8_t reg_offset = (slot == 1) ? d->op1.reg_offset : (slot == 2) ? d->op2.reg_offset : d->op3.reg_offset;
    hb_size_t sz = size_from_dec(sz_raw);
    if (is_reg) {
        int reg = (slot == 1) ? d->op1.reg : (slot == 2) ? d->op2.reg : d->op3.reg;
        hb_ir_operand_t op = hb_ir_reg(map_x86_reg_to_ir(reg), sz);
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
        hb_ir_operand_t op = hb_ir_mem_segment(
            base >= 0 ? map_x86_reg_to_ir(base) : HB_REG_COUNT,
            index >= 0 ? map_x86_reg_to_ir(index) : HB_REG_COUNT,
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
        default: return HB_CC_E;
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

hb_result_t hb_lift_x86(const hb_decoded_t* dec, hb_ir_builder_t* b) {
    if (!dec || !b) return HB_ERR_INVALID_ARG;

    switch (dec->opcode) {
        case HB_INS_MOV: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            if (dec->op1.is_mem && (dec->op2.is_reg || dec->op2.is_imm)) {
                emit(b, hb_ir_emit_store(b, dst, src), dec);
            } else if (dec->op1.is_reg && dec->op2.is_mem) {
                emit(b, hb_ir_emit_load(b, dst, src), dec);
            } else {
                emit(b, hb_ir_emit_mov(b, dst, src), dec);
            }
            return HB_OK;
        }
        case HB_INS_MOV_SEG: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_unop(b, HB_IR_MOV_SEG, dst, src), dec);
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
        case HB_INS_MOVHLPS:
        case HB_INS_MOVLHPS:
        case HB_INS_MOVHPS:
        case HB_INS_MOVHPD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_XMM_QWORD_LANE_MOV);
            if (i) {
                bool store = dec->op1.is_mem;
                unsigned dst_lane = (dec->opcode == HB_INS_MOVLHPS ||
                                     (!store && (dec->opcode == HB_INS_MOVHPS || dec->opcode == HB_INS_MOVHPD))) ? 1 : 0;
                unsigned src_lane = (dec->opcode == HB_INS_MOVHLPS ||
                                     (store && (dec->opcode == HB_INS_MOVHPS || dec->opcode == HB_INS_MOVHPD))) ? 1 : 0;
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = dst_lane | (src_lane << 8);
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
        case HB_INS_SQRTPS:
        case HB_INS_SQRTPD:
        case HB_INS_SQRTSS:
        case HB_INS_SQRTSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_FSQRT);
            if (i) {
                unsigned lane = (dec->opcode == HB_INS_SQRTPD || dec->opcode == HB_INS_SQRTSD) ? 8 : 4;
                bool scalar = (dec->opcode == HB_INS_SQRTSS || dec->opcode == HB_INS_SQRTSD);
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = lane | (scalar ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_RSQRTPS:
        case HB_INS_RSQRTSS:
        case HB_INS_RCPPS:
        case HB_INS_RCPSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            bool is_rsqrt = (dec->opcode == HB_INS_RSQRTPS || dec->opcode == HB_INS_RSQRTSS);
            bool scalar = (dec->opcode == HB_INS_RSQRTSS || dec->opcode == HB_INS_RCPSS);
            hb_ir_instr_t *i = hb_ir_emit(b, is_rsqrt ? HB_IR_FRSQRT : HB_IR_FRCP);
            if (i) {
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = 4 | (scalar ? 0x100 : 0);
            }
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
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUNPCK);
            if (i) {
                unsigned lane = 1;
                if (dec->opcode == HB_INS_PUNPCKLWD || dec->opcode == HB_INS_PUNPCKHWD) lane = 2;
                else if (dec->opcode == HB_INS_PUNPCKLDQ || dec->opcode == HB_INS_PUNPCKHDQ) lane = 4;
                else if (dec->opcode == HB_INS_PUNPCKLQDQ || dec->opcode == HB_INS_PUNPCKHQDQ) lane = 8;
                bool high = (dec->opcode == HB_INS_PUNPCKHBW || dec->opcode == HB_INS_PUNPCKHWD ||
                             dec->opcode == HB_INS_PUNPCKHDQ || dec->opcode == HB_INS_PUNPCKHQDQ);
                i->dst = operand_from_dec(dec, 1);
                i->src1 = i->dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = lane | (high ? 0x100 : 0);
            }
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
        case HB_INS_PINSRW: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PINSRW);
            if (i) {
                i->dst = dst;
                i->src1 = dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = (uint64_t)dec->op3.imm & 7u;
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PEXTRW: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PEXTRW);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = (uint64_t)dec->op3.imm & 7u;
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PADDB:
        case HB_INS_PADDW:
        case HB_INS_PADDD:
        case HB_INS_PADDQ:
        case HB_INS_PSUBB:
        case HB_INS_PSUBW:
        case HB_INS_PSUBD:
        case HB_INS_PSUBQ: {
            hb_ir_instr_t *i = hb_ir_emit(b, (dec->opcode == HB_INS_PADDB ||
                                             dec->opcode == HB_INS_PADDW ||
                                             dec->opcode == HB_INS_PADDD ||
                                             dec->opcode == HB_INS_PADDQ) ? HB_IR_PADD : HB_IR_PSUB);
            if (i) {
                unsigned lane = 1;
                if (dec->opcode == HB_INS_PADDW || dec->opcode == HB_INS_PSUBW) lane = 2;
                else if (dec->opcode == HB_INS_PADDD || dec->opcode == HB_INS_PSUBD) lane = 4;
                else if (dec->opcode == HB_INS_PADDQ || dec->opcode == HB_INS_PSUBQ) lane = 8;
                hb_ir_operand_t dst = operand_from_dec(dec, 1);
                i->dst = dst;
                i->src1 = dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = lane;
            }
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
        case HB_INS_SHUFPS:
        case HB_INS_SHUFPD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_FSHUF);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
                i->target = (dec->opcode == HB_INS_SHUFPD ? 8 : 4) |
                            (((uint64_t)dec->op3.imm & 0xffu) << 8);
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
        case HB_INS_ROR:
        case HB_INS_RCL:
        case HB_INS_RCR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_op_t ir_op = (dec->opcode == HB_INS_SHL) ? HB_IR_SHL :
                               (dec->opcode == HB_INS_SHR) ? HB_IR_SHR :
                               (dec->opcode == HB_INS_SAR) ? HB_IR_SAR :
                               (dec->opcode == HB_INS_ROL) ? HB_IR_ROL :
                               (dec->opcode == HB_INS_ROR) ? HB_IR_ROR :
                               (dec->opcode == HB_INS_RCL) ? HB_IR_RCL : HB_IR_RCR;
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
        case HB_INS_CWD: {
            /* CWD / CDQ / CQO — sign-extend EAX/AX/RAX into EDX:EAX/DX:AX/RDX:RAX.
             * op1.size carries the operand size: 2 → CWD, 4 → CDQ, 8 → CQO. */
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CWD);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CWDE: {
            /* CBW / CWDE — sign-extend AL into AX (16-bit opsize) or AX into EAX
             * (32-bit opsize). op1.size carries the source width: 1 → CBW (AL→AX),
             * 2 → CWDE (AX→EAX). Implement as SIGN_EXTEND of AX/AL into EAX. */
            hb_size_t src_size = (dec->op1.size == 1) ? HB_SIZE_8 : HB_SIZE_16;
            hb_ir_operand_t src = hb_ir_reg(HB_REG_RAX, src_size);
            hb_ir_operand_t dst = hb_ir_reg(HB_REG_RAX, HB_SIZE_32);
            hb_ir_instr_t *i = hb_ir_emit_unop(b, HB_IR_SIGN_EXTEND, dst, src);
            emit(b, i, dec);
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
        case HB_INS_PUSHF: {
            hb_ir_operand_t size_op = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUSHF);
            if (i) i->src1 = size_op;
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_POPF: {
            hb_ir_operand_t size_op = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_POPF);
            if (i) i->src1 = size_op;
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LEAVE: {
            hb_ir_operand_t sp = hb_ir_reg(HB_REG_RSP, HB_SIZE_32);
            hb_ir_operand_t bp32 = hb_ir_reg(HB_REG_RBP, HB_SIZE_32);
            hb_ir_operand_t bp = operand_from_dec(dec, 1);

            emit(b, hb_ir_emit_mov(b, sp, bp32), dec);
            emit(b, hb_ir_emit_pop(b, bp), dec);
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
        case HB_INS_LOOP:
        case HB_INS_JRCXZ: {
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_LOOP ? HB_IR_LOOP : HB_IR_JRCXZ);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = dec->branch_target;
            }
            emit(b, i, dec);
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
        case HB_INS_MOVZX: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_ZERO_EXTEND);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVSX: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_SIGN_EXTEND);
            if (i) { i->dst = dst; i->src1 = src; }
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
        case HB_INS_STOS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_STOS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_BSF:
        case HB_INS_TZCNT: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_BSF ? HB_IR_BSF : HB_IR_TZCNT);
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
        case HB_INS_MULPS:
        case HB_INS_MULPD:
        case HB_INS_DIVPS:
        case HB_INS_DIVPD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            bool is_div = (dec->opcode == HB_INS_DIVPS || dec->opcode == HB_INS_DIVPD);
            hb_ir_instr_t *i = hb_ir_emit(b, is_div ? HB_IR_FDIV : HB_IR_FMUL);
            if (i) {
                unsigned lane = (dec->opcode == HB_INS_MULPD || dec->opcode == HB_INS_DIVPD) ? 8 : 4;
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = lane;
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
        case HB_INS_MULSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MULSD);
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
        case HB_INS_MULSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MULSS);
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
        case HB_INS_COMISS:
        case HB_INS_COMISD: {
            hb_ir_operand_t lhs = operand_from_dec(dec, 1);
            hb_ir_operand_t rhs = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_COMISD ? HB_IR_COMISD : HB_IR_COMISS);
            if (i) { i->src1 = lhs; i->src2 = rhs; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_NOP: {
            emit(b, hb_ir_emit(b, HB_IR_NOP), dec);
            return HB_OK;
        }
        case HB_INS_X87_FLD:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FLD, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FST:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FST, operand_from_dec(dec, 1), hb_ir_none()), dec);
            return HB_OK;
        case HB_INS_X87_FSTP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FSTP, operand_from_dec(dec, 1), hb_ir_none()), dec);
            return HB_OK;
        case HB_INS_X87_FILD:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FILD, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FISTP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FISTP, operand_from_dec(dec, 1), hb_ir_none()), dec);
            return HB_OK;
        case HB_INS_X87_FLDCW:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FLDCW, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FNSTCW:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FNSTCW, operand_from_dec(dec, 1), hb_ir_none()), dec);
            return HB_OK;
        case HB_INS_X87_FNSTSW:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FNSTSW, operand_from_dec(dec, 1), hb_ir_none()), dec);
            return HB_OK;
        case HB_INS_X87_FADD:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FADD, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FMUL:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FMUL, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FCOM:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FCOM, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FCOMP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FCOMP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FSUB:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FSUB, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FSUBR:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FSUBR, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FDIV:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FDIV, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FDIVR:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FDIVR, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FADDP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FADDP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FMULP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FMULP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FCOMPP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FCOMPP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FSUBP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FSUBP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FSUBRP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FSUBRP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FDIVP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FDIVP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FDIVRP:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FDIVRP, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FXCH:
            emit(b, hb_ir_emit_unop(b, HB_IR_X87_FXCH, hb_ir_none(), operand_from_dec(dec, 1)), dec);
            return HB_OK;
        case HB_INS_X87_FRNDINT:
            emit(b, hb_ir_emit(b, HB_IR_X87_FRNDINT), dec);
            return HB_OK;
        case HB_INS_X87_FUCOM:
        case HB_INS_X87_FUCOMP:
        case HB_INS_X87_FUCOMI:
        case HB_INS_X87_FUCOMPI:
            /* FUCOM/FUCOMP/FUCOMI/FUCOMPI: unordered compares. Model as a
             * no-op FNINIT for now — these are 0-side-effect at the IR level
             * because FPU condition codes are not yet exposed. */
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);
            return HB_OK;
        case HB_INS_X87_FNCLEX:
            emit(b, hb_ir_emit(b, HB_IR_X87_FNCLEX), dec);
            return HB_OK;
        case HB_INS_X87_FNINIT:
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);
            return HB_OK;
        case HB_INS_X87_FFREE:
            /* FFREE ST(i): mark ST(i) as free. We model as a no-op since the
             * stack pointer is implicit in our interpreter. */
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);  /* stand-in */
            return HB_OK;
        case HB_INS_X87_MISC:
            /* FNOP and other FPU no-op-style opcodes. We model as a no-op;
             * the FNINIT stand-in works because both are 0-side-effect FPU
             * ops at the IR level. */
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);  /* stand-in */
            return HB_OK;
        case HB_INS_X87_FFREEP:
            /* FFREEP ST(i): pop + free. Stand-in via FNINIT. */
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);  /* stand-in */
            return HB_OK;
        case HB_INS_X87_FCMOV:
            /* FCMOVcc ST, ST(i): conditional move based on EFLAGS. We
             * currently implement as FNINIT (no-op) since condition code is
             * not yet tracked per FCMOV variant. */
            emit(b, hb_ir_emit(b, HB_IR_X87_FNINIT), dec);
            return HB_OK;
        case HB_INS_PUSHA: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUSHA);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_POPA: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_POPA);
            if (i) i->dst = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_AAA:
            emit(b, hb_ir_emit(b, HB_IR_AAA), dec);
            return HB_OK;
        case HB_INS_AAS:
            emit(b, hb_ir_emit(b, HB_IR_AAS), dec);
            return HB_OK;
        case HB_INS_AAM: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_AAM);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_AAD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_AAD);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DAA:
            emit(b, hb_ir_emit(b, HB_IR_DAA), dec);
            return HB_OK;
        case HB_INS_DAS:
            emit(b, hb_ir_emit(b, HB_IR_DAS), dec);
            return HB_OK;
        case HB_INS_BOUND: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_BOUND);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_ARPL: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_ARPL);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LDS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LDS);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LES: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LES);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LFS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LFS);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LGS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LGS);
            if (i) { i->dst = operand_from_dec(dec, 1); i->src1 = operand_from_dec(dec, 2); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PUSH_SEG: {
            /* op1.size = element size, op2.imm = segment selector. */
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUSH_SEG);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_POP_SEG: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_POP_SEG);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CLC:
            emit(b, hb_ir_emit(b, HB_IR_CLC), dec);
            return HB_OK;
        case HB_INS_STC:
            emit(b, hb_ir_emit(b, HB_IR_STC), dec);
            return HB_OK;
        case HB_INS_CMC:
            emit(b, hb_ir_emit(b, HB_IR_CMC), dec);
            return HB_OK;
        case HB_INS_CLD:
            emit(b, hb_ir_emit(b, HB_IR_CLD), dec);
            return HB_OK;
        case HB_INS_STD:
            emit(b, hb_ir_emit(b, HB_IR_STD), dec);
            return HB_OK;
        case HB_INS_CLI:
            emit(b, hb_ir_emit(b, HB_IR_CLI), dec);
            return HB_OK;
        case HB_INS_STI:
            emit(b, hb_ir_emit(b, HB_IR_STI), dec);
            return HB_OK;
        case HB_INS_IRET: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_IRET);
            if (i) i->src1 = operand_from_dec(dec, 1); /* size (16 vs 32) */
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_INT3:
            emit(b, hb_ir_emit(b, HB_IR_INT3), dec);
            return HB_OK;
        case HB_INS_INT1:
            emit(b, hb_ir_emit(b, HB_IR_INT1), dec);
            return HB_OK;
        case HB_INS_INT: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_INT);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_INTO:
            emit(b, hb_ir_emit(b, HB_IR_INTO), dec);
            return HB_OK;
        case HB_INS_RETF: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_RETF);
            if (i) i->src1 = hb_ir_imm(dec->ret_imm, HB_SIZE_16);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_XLAT:
            emit(b, hb_ir_emit(b, HB_IR_XLAT), dec);
            return HB_OK;
        case HB_INS_SALC: {
            /* SALC (undocumented 0xD6): AL = 0xFF if CF=1 else 0x00.
             * Implemented as SETB AL. */
            hb_ir_operand_t al = hb_ir_reg(HB_REG_RAX, HB_SIZE_8);
            emit(b, hb_ir_emit_setcc(b, HB_CC_B, al), dec);
            return HB_OK;
        }
        case HB_INS_ENTER: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_ENTER);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_HLT:
            /* Privileged: lift as fault so the user knows. */
            emit(b, hb_ir_emit_fault(b, -8, "HLT in user-mode guest"), dec);
            return HB_OK;
        case HB_INS_IN:
            /* Privileged port I/O. Lift as fault. */
            emit(b, hb_ir_emit_fault(b, -6, "IN port I/O in user-mode guest"), dec);
            return HB_OK;
        case HB_INS_OUT:
            emit(b, hb_ir_emit_fault(b, -6, "OUT port I/O in user-mode guest"), dec);
            return HB_OK;
        case HB_INS_INS:
        case HB_INS_OUTS:
            /* String port I/O (INSB/INSW/INSD/OUTSB/OUTSW/OUTSD). Privileged;
             * lift as fault. */
            emit(b, hb_ir_emit_fault(b, -6,
                (dec->opcode == HB_INS_INS) ? "INS port I/O in user-mode guest"
                                            : "OUTS port I/O in user-mode guest"), dec);
            return HB_OK;
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
        hb_result_t r = hb_lift_x86(&d, b);
        if (r != HB_OK && r != HB_ERR_UNSUPPORTED_FEATURE) {
            hb_ir_builder_destroy(b);
            hb_ir_func_destroy(func);
            return r;
        }
        count++;
        if (d.is_branch || d.is_ret || d.is_call) break;
        if (count >= instr_limit) {
            if (dec->pos < dec->code_len) {
                func->truncated = true;
                func->truncation_reason = "x86 lifter instruction limit";
            }
            break;
        }
    }

    hb_ir_builder_destroy(b);
    *out = func;
    return HB_OK;
}
