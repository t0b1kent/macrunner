#include "hb_decoder.h"
#include "hb_ir.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const uint8_t* code;
    size_t len;
    size_t pos;
    uint64_t addr;
} hb_dec_t;

static inline bool can_read(hb_dec_t* d, size_t n) {
    return d->pos + n <= d->len;
}

static inline uint8_t read_u8(hb_dec_t* d) {
    return d->code[d->pos++];
}

static inline int8_t read_s8(hb_dec_t* d) {
    return (int8_t)d->code[d->pos++];
}

static inline int32_t read_s32(hb_dec_t* d) {
    uint32_t lo = d->code[d->pos] | (d->code[d->pos+1] << 8u)
                 | (d->code[d->pos+2] << 16u) | (d->code[d->pos+3] << 24u);
    d->pos += 4;
    return (int32_t)lo;
}

static inline void set_reg(hb_decoded_t* out, int slot, int r, uint8_t sz) {
    if (slot == 1) {
        out->op1.present = true;
        out->op1.is_reg = true;
        out->op1.reg = r;
        out->op1.size = sz;
    } else if (slot == 2) {
        out->op2.present = true;
        out->op2.is_reg = true;
        out->op2.reg = r;
        out->op2.size = sz;
    } else if (slot == 3) {
        out->op3.present = true;
        out->op3.is_reg = true;
        out->op3.reg = r;
        out->op3.size = sz;
    }
}

static inline void set_imm(hb_decoded_t* out, int slot, int64_t v, uint8_t sz) {
    if (slot == 1) {
        out->op1.present = true;
        out->op1.is_imm = true;
        out->op1.imm = v;
        out->op1.size = sz;
    } else if (slot == 2) {
        out->op2.present = true;
        out->op2.is_imm = true;
        out->op2.imm = v;
        out->op2.size = sz;
    } else if (slot == 3) {
        out->op3.present = true;
        out->op3.is_imm = true;
        out->op3.imm = v;
        out->op3.size = sz;
    }
}

static inline void set_mem(hb_decoded_t* out, int slot,
                           int base, int index, uint8_t scale, int64_t disp, uint8_t sz) {
    if (slot == 1) {
        out->op1.present = true;
        out->op1.is_mem = true;
        out->op1.mem.base = base;
        out->op1.mem.index = index;
        out->op1.mem.scale = scale;
        out->op1.mem.disp = disp;
        out->op1.size = sz;
    } else if (slot == 2) {
        out->op2.present = true;
        out->op2.is_mem = true;
        out->op2.mem.base = base;
        out->op2.mem.index = index;
        out->op2.mem.scale = scale;
        out->op2.mem.disp = disp;
        out->op2.size = sz;
    } else if (slot == 3) {
        out->op3.present = true;
        out->op3.is_mem = true;
        out->op3.mem.base = base;
        out->op3.mem.index = index;
        out->op3.mem.scale = scale;
        out->op3.mem.disp = disp;
        out->op3.size = sz;
    }
}

static hb_result_t parse_modrm(hb_dec_t* d, uint8_t modrm,
                               uint8_t def_size, hb_decoded_t* out,
                               int dst_slot, int src_slot,
                               bool mem_is_dst) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg_op = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;

    out->has_modrm = true;
    out->mod = mod;
    out->reg_op = reg_op;
    out->rm = rm;

    int reg = reg_op;
    uint8_t sz = def_size;

    if (mod == 3) {
        int rm_reg = rm;
        if (mem_is_dst) {
            set_reg(out, dst_slot, rm_reg, sz);
            set_reg(out, src_slot, reg, sz);
        } else {
            set_reg(out, dst_slot, reg, sz);
            set_reg(out, src_slot, rm_reg, sz);
        }
        return HB_OK;
    }

    int base = -1, index = -1;
    uint8_t scale = 1;
    int64_t disp = 0;

    if (rm == 4) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t sib = read_u8(d);
        out->has_sib = true;
        out->sib_scale = (sib >> 6) & 3;
        out->sib_index = (sib >> 3) & 7;
        out->sib_base = sib & 7;
        scale = (uint8_t)(1u << out->sib_scale);
        int si = out->sib_index;
        if (out->sib_index == 4) index = -1; else index = si;
        int sb = out->sib_base;
        if (out->sib_base == 5) {
            if (mod == 0) base = -1; else base = sb;
        } else {
            base = sb;
        }
    } else if (rm == 5 && mod == 0) {
        /* disp32 only, no base */
        base = -1;
    } else {
        base = rm;
    }

    if (mod == 1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        disp = read_s8(d);
    } else if (mod == 2 || (rm == 5 && mod == 0) || (rm == 4 && out->sib_base == 5 && mod == 0)) {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        disp = read_s32(d);
    }

    if (mem_is_dst) {
        set_mem(out, dst_slot, base, index, scale, disp, sz);
        set_reg(out, src_slot, reg, sz);
    } else {
        set_reg(out, dst_slot, reg, sz);
        set_mem(out, src_slot, base, index, scale, disp, sz);
    }
    return HB_OK;
}

static hb_result_t parse_modrm_ext(hb_dec_t* d, uint8_t modrm,
                                   uint8_t def_size, hb_decoded_t* out,
                                   int op_slot) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    out->has_modrm = true;
    out->mod = mod;
    out->rm = rm;
    uint8_t sz = def_size;

    if (mod == 3) {
        set_reg(out, op_slot, rm, sz);
        return HB_OK;
    }

    int base = -1, index = -1;
    uint8_t scale = 1;
    int64_t disp = 0;

    if (rm == 4) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t sib = read_u8(d);
        out->has_sib = true;
        out->sib_scale = (sib >> 6) & 3;
        out->sib_index = (sib >> 3) & 7;
        out->sib_base = sib & 7;
        scale = (uint8_t)(1u << out->sib_scale);
        int si = out->sib_index;
        if (out->sib_index == 4) index = -1; else index = si;
        int sb = out->sib_base;
        if (out->sib_base == 5) {
            if (mod == 0) base = -1; else base = sb;
        } else {
            base = sb;
        }
    } else if (rm == 5 && mod == 0) {
        base = -1;
    } else {
        base = rm;
    }

    if (mod == 1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        disp = read_s8(d);
    } else if (mod == 2 || (rm == 5 && mod == 0) || (rm == 4 && out->sib_base == 5 && mod == 0)) {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        disp = read_s32(d);
    }

    set_mem(out, op_slot, base, index, scale, disp, sz);
    return HB_OK;
}

static int cond_from_cc(uint8_t cc) {
    switch (cc) {
        case 0: return HB_COND_O;
        case 1: return HB_COND_NO;
        case 2: return HB_COND_B;
        case 3: return HB_COND_AE;
        case 4: return HB_COND_E;
        case 5: return HB_COND_NE;
        case 6: return HB_COND_BE;
        case 7: return HB_COND_A;
        case 8: return HB_COND_S;
        case 9: return HB_COND_NS;
        case 10: return HB_COND_P;
        case 11: return HB_COND_NP;
        case 12: return HB_COND_L;
        case 13: return HB_COND_GE;
        case 14: return HB_COND_LE;
        case 15: return HB_COND_G;
        default: return HB_COND_NONE;
    }
}

static hb_result_t decode_one(hb_dec_t* d, hb_decoded_t* out) {
    if (d->pos >= d->len) return HB_ERR_DECODE_FAILED;

    uint64_t addr = d->addr;

    memset(out, 0, sizeof(hb_decoded_t));
    out->addr = addr;

    uint8_t opcode = 0;

    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    opcode = read_u8(d);

    /* Group: MOV */
    if (opcode == 0x88) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x89) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x8A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x8B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }
    if (opcode == 0x8D) {
        /* LEA r32, m */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_LEA;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }
    if (opcode >= 0xB0 && opcode <= 0xB7) {
        /* MOV r8, imm8 */
        int reg = opcode & 7;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t imm = read_s8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        set_reg(out, 1, reg, 1);
        set_imm(out, 2, imm, 1);
        return HB_OK;
    }
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        /* MOV r32, imm32 */
        int reg = opcode & 7;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        set_reg(out, 1, reg, 4);
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }
    if (opcode == 0xC6) {
        /* MOV r/m8, imm8 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        hb_result_t r = parse_modrm_ext(d, modrm, 1, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0xC7) {
        /* MOV r/m32, imm32 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        hb_result_t r = parse_modrm_ext(d, modrm, 4, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }

    /* Group: ADD */
    if (opcode == 0x00) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x01) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x02) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x03) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }
    if (opcode == 0x04) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        set_reg(out, 1, 0, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x05) {
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, 4);
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }

    /* Group: SUB */
    if (opcode == 0x28) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x29) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x2A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x2B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }
    if (opcode == 0x2C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        set_reg(out, 1, 0, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x2D) {
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, 4);
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }

    /* Group: CMP */
    if (opcode == 0x38) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x39) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x3A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x3B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }
    if (opcode == 0x3C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        set_reg(out, 1, 0, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x3D) {
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, 4);
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }

    /* Group: PUSH / POP */
    if (opcode >= 0x50 && opcode <= 0x57) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -4;
        set_reg(out, 1, opcode & 7, 4);
        return HB_OK;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        out->opcode = HB_INS_POP;
        out->stack_delta = 4;
        set_reg(out, 1, opcode & 7, 4);
        return HB_OK;
    }
    if (opcode == 0x68) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -4;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 1, (int64_t)read_s32(d), 4);
        return HB_OK;
    }
    if (opcode == 0x6A) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -4;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 1, (int64_t)read_s8(d), 1);
        return HB_OK;
    }

    /* Group: INC reg (0x40-0x47) -- in x86 these are real INC, not REX */
    if (opcode >= 0x40 && opcode <= 0x47) {
        out->opcode = HB_INS_INC;
        out->writes_flags = true;
        set_reg(out, 1, opcode & 7, 4);
        return HB_OK;
    }
    /* Group: DEC reg (0x48-0x4F) -- in x86 these are real DEC, not REX */
    if (opcode >= 0x48 && opcode <= 0x4F) {
        out->opcode = HB_INS_DEC;
        out->writes_flags = true;
        set_reg(out, 1, opcode & 7, 4);
        return HB_OK;
    }

    /* Group: JMP */
    if (opcode == 0xEB) {
        out->opcode = HB_INS_JMP;
        out->is_branch = true;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t rel = read_s8(d);
        out->branch_target = addr + d->pos + rel;
        return HB_OK;
    }
    if (opcode == 0xE9) {
        out->opcode = HB_INS_JMP;
        out->is_branch = true;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        int32_t rel = read_s32(d);
        out->branch_target = addr + d->pos + rel;
        return HB_OK;
    }

    /* Group: CALL */
    if (opcode == 0xE8) {
        out->opcode = HB_INS_CALL;
        out->is_call = true;
        out->stack_delta = -4;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        int32_t rel = read_s32(d);
        out->branch_target = addr + d->pos + rel;
        return HB_OK;
    }

    /* Group: RET */
    if (opcode == 0xC3) {
        out->opcode = HB_INS_RET;
        out->is_ret = true;
        out->stack_delta = 4;
        return HB_OK;
    }
    if (opcode == 0xC2) {
        out->opcode = HB_INS_RET;
        out->is_ret = true;
        out->stack_delta = 4;
        if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
        uint16_t imm = (uint16_t)(read_u8(d) | (read_u8(d) << 8));
        out->ret_imm = imm;
        return HB_OK;
    }

    /* Group: Jcc short */
    if (opcode >= 0x70 && opcode <= 0x7F) {
        out->opcode = HB_INS_Jcc;
        out->is_branch = true;
        out->is_conditional = true;
        out->cond = cond_from_cc(opcode & 0x0F);
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t rel = read_s8(d);
        out->branch_target = addr + d->pos + rel;
        out->reads_flags = true;
        return HB_OK;
    }

    /* Group: NOP */
    if (opcode == 0x90) {
        out->opcode = HB_INS_NOP;
        return HB_OK;
    }

    /* Two-byte opcode: 0x0F ... */
    if (opcode == 0x0F) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        /* Jcc near */
        if (op2 >= 0x80 && op2 <= 0x8F) {
            out->opcode = HB_INS_Jcc;
            out->is_branch = true;
            out->is_conditional = true;
            out->cond = cond_from_cc(op2 & 0x0F);
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            int32_t rel = read_s32(d);
            out->branch_target = addr + d->pos + rel;
            out->reads_flags = true;
            return HB_OK;
        }
        if (op2 >= 0x90 && op2 <= 0x9F) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SETcc;
            out->cond = cond_from_cc(op2 & 0x0F);
            out->reads_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, 1, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 >= 0x40 && op2 <= 0x4F) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CMOVcc;
            out->cond = cond_from_cc(op2 & 0x0F);
            out->reads_flags = true;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    /* Group: 0x80/0x81/0x83 immediate group */
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        uint8_t sz = (opcode == 0x80) ? 1 : 4;

        if (ext == 0) out->opcode = HB_INS_ADD;
        else if (ext == 5) out->opcode = HB_INS_SUB;
        else if (ext == 7) out->opcode = HB_INS_CMP;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;

        hb_result_t r = parse_modrm_ext(d, modrm, sz == 1 ? 1 : 4, out, 1);
        if (r != HB_OK) return r;

        if (opcode == 0x80) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_s8(d), 1);
        } else if (opcode == 0x81) {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s32(d), sz);
        } else { /* 0x83 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s8(d), sz);
        }
        return HB_OK;
    }

    /* Group: SHL/SHR/SAR by imm8 (C1) */
    if (opcode == 0xC1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        hb_result_t r = parse_modrm_ext(d, modrm, 4, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_u8(d) & 0x1F, 1);
        return HB_OK;
    }

    /* Group: SHL/SHR/SAR by 1 (D1) */
    if (opcode == 0xD1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        hb_result_t r = parse_modrm_ext(d, modrm, 4, out, 1);
        if (r != HB_OK) return r;
        set_imm(out, 2, 1, 1);
        return HB_OK;
    }

    /* Group: SHL/SHR/SAR by CL (D3) */
    if (opcode == 0xD3) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        hb_result_t r = parse_modrm_ext(d, modrm, 4, out, 1);
        if (r != HB_OK) return r;
        set_reg(out, 2, HB_REG_X86_ECX, 1); /* CL */
        return HB_OK;
    }

    /* Group: NOT / NEG (F7) */
    if (opcode == 0xF7) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 2) out->opcode = HB_INS_NOT;
        else if (ext == 3) out->opcode = HB_INS_NEG;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = (ext == 3);
        return parse_modrm_ext(d, modrm, 4, out, 1);
    }

    /* Group: 0xFF */
    if (opcode == 0xFF) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) {
            out->opcode = HB_INS_INC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, 4, out, 1);
        }
        if (ext == 1) {
            out->opcode = HB_INS_DEC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, 4, out, 1);
        }
        if (ext == 2) {
            out->opcode = HB_INS_CALL;
            out->is_call = true;
            out->stack_delta = -4;
            return parse_modrm_ext(d, modrm, 4, out, 1);
        }
        if (ext == 4) {
            out->opcode = HB_INS_JMP;
            out->is_branch = true;
            return parse_modrm_ext(d, modrm, 4, out, 1);
        }
        if (ext == 6) {
            out->opcode = HB_INS_PUSH;
            out->stack_delta = -4;
            return parse_modrm_ext(d, modrm, 4, out, 1);
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    /* TEST (subset) */
    if (opcode == 0x84) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x85) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0xA8) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        set_reg(out, 1, 0, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0xA9) {
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, 4);
        set_imm(out, 2, (int64_t)read_s32(d), 4);
        return HB_OK;
    }

    /* AND (subset) */
    if (opcode == 0x20) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x21) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x22) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x23) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }

    /* OR (subset) */
    if (opcode == 0x08) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x09) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x0A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x0B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }

    /* XOR (subset) */
    if (opcode == 0x30) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, true);
    }
    if (opcode == 0x31) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, true);
    }
    if (opcode == 0x32) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, 1, out, 1, 2, false);
    }
    if (opcode == 0x33) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, 4, out, 1, 2, false);
    }

    /* Unsupported */
    return HB_ERR_UNSUPPORTED_OPCODE;
}

hb_result_t hb_decode_x86(const uint8_t* code, size_t len, uint64_t addr, hb_decoded_t* out) {
    if (!code || !out || len == 0) return HB_ERR_INVALID_ARG;

    hb_dec_t d = { code, len, 0, addr };
    hb_result_t r = decode_one(&d, out);
    if (r != HB_OK) {
        out->len = (uint8_t)(d.pos > 0 ? d.pos : 1);
        if (r == HB_ERR_UNSUPPORTED_OPCODE) out->opcode = HB_INS_UNSUPPORTED;
        memcpy(out->bytes, code, out->len > 15 ? 15 : out->len);
        return r;
    }

    out->len = (uint8_t)d.pos;
    memcpy(out->bytes, code, out->len > 15 ? 15 : out->len);
    return HB_OK;
}
