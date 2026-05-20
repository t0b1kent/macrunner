#include "hb_decoder.h"
#include "hb_ir.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const uint8_t* code;
    size_t len;
    size_t pos;
    uint64_t addr;
    bool fault;
} hb_dec_t;

static inline bool can_read(hb_dec_t* d, size_t n) {
    return d->pos + n <= d->len;
}

static inline uint8_t read_u8(hb_dec_t* d) {
    if (!can_read(d, 1)) {
        d->fault = true;
        d->pos = d->len;
        return 0xCC;
    }
    return d->code[d->pos++];
}

static inline int8_t read_s8(hb_dec_t* d) {
    return (int8_t)d->code[d->pos++];
}

static inline int16_t read_s16(hb_dec_t* d) {
    uint16_t lo = d->code[d->pos] | (d->code[d->pos+1] << 8u);
    d->pos += 2;
    return (int16_t)lo;
}

static inline int32_t read_s32(hb_dec_t* d) {
    uint32_t lo = d->code[d->pos] | (d->code[d->pos+1] << 8u)
                 | (d->code[d->pos+2] << 16u) | (d->code[d->pos+3] << 24u);
    d->pos += 4;
    return (int32_t)lo;
}

static inline uint64_t read_u64(hb_dec_t* d) {
    uint64_t a = d->code[d->pos] | ((uint64_t)d->code[d->pos+1] << 8u)
               | ((uint64_t)d->code[d->pos+2] << 16u) | ((uint64_t)d->code[d->pos+3] << 24u);
    d->pos += 4;
    uint64_t b = d->code[d->pos] | ((uint64_t)d->code[d->pos+1] << 8u)
               | ((uint64_t)d->code[d->pos+2] << 16u) | ((uint64_t)d->code[d->pos+3] << 24u);
    d->pos += 4;
    return a | (b << 32);
}

static inline int reg_idx(int base, bool rex_bit) {
    return base | (rex_bit ? 8 : 0);
}

static inline int reg8_idx(int base, bool has_rex, bool rex_bit, uint8_t* byte_offset) {
    if (byte_offset) *byte_offset = 0;
    if (!has_rex && base >= 4 && base <= 7) {
        if (byte_offset) *byte_offset = 1;
        return base - 4; /* AH/CH/DH/BH alias byte 1 of RAX/RCX/RDX/RBX. */
    }
    return reg_idx(base, rex_bit);
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

static inline void set_reg_ex(hb_decoded_t* out, int slot, int r, uint8_t sz, uint8_t reg_offset) {
    set_reg(out, slot, r, sz);
    if (slot == 1) out->op1.reg_offset = reg_offset;
    else if (slot == 2) out->op2.reg_offset = reg_offset;
    else if (slot == 3) out->op3.reg_offset = reg_offset;
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
        out->op1.mem.segment = out->segment_prefix;
        out->op1.mem.addr32 = out->address32_prefix;
        out->op1.size = sz;
    } else if (slot == 2) {
        out->op2.present = true;
        out->op2.is_mem = true;
        out->op2.mem.base = base;
        out->op2.mem.index = index;
        out->op2.mem.scale = scale;
        out->op2.mem.disp = disp;
        out->op2.mem.segment = out->segment_prefix;
        out->op2.mem.addr32 = out->address32_prefix;
        out->op2.size = sz;
    } else if (slot == 3) {
        out->op3.present = true;
        out->op3.is_mem = true;
        out->op3.mem.base = base;
        out->op3.mem.index = index;
        out->op3.mem.scale = scale;
        out->op3.mem.disp = disp;
        out->op3.mem.segment = out->segment_prefix;
        out->op3.mem.addr32 = out->address32_prefix;
        out->op3.size = sz;
    }
}

/* x86_64 RIP-relative memory operands use RIP after the entire instruction.
 * Immediate-form opcodes parse ModRM/disp32 before trailing imm bytes, so
 * refresh after reading the imm or C7/81/F7/69/C1/0F BA target too early.
 */
static inline void refresh_rip_targets(hb_dec_t* d, hb_decoded_t* out) {
    uint64_t rip_target = d->addr + d->pos;
    if (out->op1.is_mem && out->op1.mem.rip_relative) out->op1.mem.rip_target = rip_target;
    if (out->op2.is_mem && out->op2.mem.rip_relative) out->op2.mem.rip_target = rip_target;
    if (out->op3.is_mem && out->op3.mem.rip_relative) out->op3.mem.rip_target = rip_target;
}

static inline void mark_xmm_operand(hb_decoded_t* out, int slot) {
    if (slot == 1 && out->op1.is_reg) {
        out->op1.reg += HB_REG_XMM0;
        out->op1.size = 16;
    } else if (slot == 2 && out->op2.is_reg) {
        out->op2.reg += HB_REG_XMM0;
        out->op2.size = 16;
    } else if (slot == 3 && out->op3.is_reg) {
        out->op3.reg += HB_REG_XMM0;
        out->op3.size = 16;
    }
}

static inline void mark_xmm_operands(hb_decoded_t* out) {
    mark_xmm_operand(out, 1);
    mark_xmm_operand(out, 2);
    mark_xmm_operand(out, 3);
}

static hb_result_t parse_modrm(hb_dec_t* d, uint8_t modrm,
                               bool rex_w, bool rex_r, bool rex_x, bool rex_b,
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

    uint8_t sz = rex_w ? 8 : def_size;
    uint8_t reg_offset = 0;
    int reg = (sz == 1) ? reg8_idx(reg_op, out->has_rex, rex_r, &reg_offset)
                         : reg_idx(reg_op, rex_r);

    if (mod == 3) {
        uint8_t rm_offset = 0;
        int rm_reg = (sz == 1) ? reg8_idx(rm, out->has_rex, rex_b, &rm_offset)
                               : reg_idx(rm, rex_b);
        if (mem_is_dst) {
            set_reg_ex(out, dst_slot, rm_reg, sz, rm_offset);
            set_reg_ex(out, src_slot, reg, sz, reg_offset);
        } else {
            set_reg_ex(out, dst_slot, reg, sz, reg_offset);
            set_reg_ex(out, src_slot, rm_reg, sz, rm_offset);
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
        int si = reg_idx(out->sib_index, rex_x);
        if (out->sib_index == 4 && !rex_x) index = -1; else index = si;
        int sb = reg_idx(out->sib_base, rex_b);
        if (out->sib_base == 5) {
            if (mod == 0) base = -1; else base = sb;
        } else {
            base = sb;
        }
    } else if (rm == 5 && mod == 0 && !out->address32_prefix) {
        base = HB_REG_RIP;
    } else {
        base = reg_idx(rm, rex_b);
    }

    if (mod == 1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        disp = read_s8(d);
    } else if (mod == 2 || (rm == 5 && mod == 0) || (rm == 4 && out->sib_base == 5 && mod == 0)) {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        disp = read_s32(d);
    }

    if (base == HB_REG_RIP && !out->address32_prefix) {
        if (mem_is_dst) out->op1.mem.rip_relative = true;
        else out->op2.mem.rip_relative = true;
        uint64_t rip_target = d->addr + d->pos; /* d->pos is already after disp32. */
        if (mem_is_dst) out->op1.mem.rip_target = rip_target;
        else out->op2.mem.rip_target = rip_target;
    }

    if (mem_is_dst) {
        set_mem(out, dst_slot, base, index, scale, disp, sz);
        set_reg_ex(out, src_slot, reg, sz, reg_offset);
    } else {
        set_reg_ex(out, dst_slot, reg, sz, reg_offset);
        set_mem(out, src_slot, base, index, scale, disp, sz);
    }
    return HB_OK;
}

static hb_result_t parse_modrm_ext(hb_dec_t* d, uint8_t modrm,
                                   bool rex_w, bool rex_b,
                                   uint8_t def_size, hb_decoded_t* out,
                                   int op_slot) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    out->has_modrm = true;
    out->mod = mod;
    out->rm = rm;
    uint8_t sz = rex_w ? 8 : def_size;

    if (mod == 3) {
        uint8_t rm_offset = 0;
        int rm_reg = (sz == 1) ? reg8_idx(rm, out->has_rex, rex_b, &rm_offset)
                               : reg_idx(rm, rex_b);
        set_reg_ex(out, op_slot, rm_reg, sz, rm_offset);
        return HB_OK;
    }

    int base = -1, index = -1;
    uint8_t scale = 1;
    int64_t disp = 0;
    bool rex_x = out->rex_x > 0;

    if (rm == 4) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t sib = read_u8(d);
        out->has_sib = true;
        out->sib_scale = (sib >> 6) & 3;
        out->sib_index = (sib >> 3) & 7;
        out->sib_base = sib & 7;
        scale = (uint8_t)(1u << out->sib_scale);
        int si = reg_idx(out->sib_index, rex_x);
        if (out->sib_index == 4 && !rex_x) index = -1; else index = si;
        int sb = reg_idx(out->sib_base, rex_b);
        if (out->sib_base == 5) {
            if (mod == 0) base = -1; else base = sb;
        } else {
            base = sb;
        }
    } else if (rm == 5 && mod == 0 && !out->address32_prefix) {
        base = HB_REG_RIP;
        if (op_slot == 1) out->op1.mem.rip_relative = true;
        else if (op_slot == 2) out->op2.mem.rip_relative = true;
    } else {
        base = reg_idx(rm, rex_b);
    }

    if (mod == 1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        disp = read_s8(d);
    } else if (mod == 2 || (rm == 5 && mod == 0) || (rm == 4 && out->sib_base == 5 && mod == 0)) {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        disp = read_s32(d);
    }

    if (base == HB_REG_RIP && !out->address32_prefix) {
        uint64_t rip_target = d->addr + d->pos;
        if (op_slot == 1) out->op1.mem.rip_target = rip_target;
        else if (op_slot == 2) out->op2.mem.rip_target = rip_target;
    }

    set_mem(out, op_slot, base, index, scale, disp, sz);
    return HB_OK;
}

static hb_result_t set_acc_imm_op(hb_dec_t* d, hb_decoded_t* out,
                                  bool rex_w, bool operand16) {
    uint8_t sz = rex_w ? 8 : (operand16 ? 2 : 4);
    set_reg(out, 1, HB_REG_RAX, sz);
    if (operand16 && !rex_w) {
        if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, (int64_t)read_s16(d), sz);
    } else {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, (int64_t)read_s32(d), sz);
    }
    return HB_OK;
}

static int cond_from_cc(uint8_t cc) {
    switch (cc) {
        case 0: return HB_COND_O;
        case 1: return HB_COND_NO;
        case 2: return HB_COND_B; /* also C */
        case 3: return HB_COND_AE; /* also NC */
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

    bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    bool has_rex = false;
    bool operand16 = false;
    bool address32 = false;
    bool prefix_f2 = false;
    bool prefix_f3 = false;
    uint8_t opcode = 0;

    /* Legacy prefixes used by real PE prologues. FS/GS are semantically
       meaningful on Windows and are carried into IR memory operands. */
    while (can_read(d, 1)) {
        uint8_t b = d->code[d->pos];
        if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e ||
            b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3)
        {
            if (b == 0x66) operand16 = true;
            else if (b == 0x67) address32 = true;
            else if (b == 0xf2) prefix_f2 = true;
            else if (b == 0xf3) prefix_f3 = true;
            else if (b == 0x64 || b == 0x65) out->segment_prefix = b;
            d->pos++;
        }
        else break;
    }

    /* REX prefix scan (0x40-0x4F) */
    while (can_read(d, 1)) {
        uint8_t b = d->code[d->pos];
        if (b >= 0x40 && b <= 0x4F) {
            has_rex = true;
            rex_w = (b >> 3) & 1;
            rex_r = (b >> 2) & 1;
            rex_x = (b >> 1) & 1;
            rex_b = b & 1;
            d->pos++;
        } else {
            break;
        }
    }

    out->has_rex = has_rex;
    out->rex_w = rex_w ? 1 : 0;
    out->rex_r = rex_r ? 1 : 0;
    out->rex_x = rex_x ? 1 : 0;
    out->rex_b = rex_b ? 1 : 0;
    out->address32_prefix = address32;

    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    opcode = read_u8(d);
    uint8_t op_size = rex_w ? 8 : (operand16 ? 2 : 4);

    /* Group: MOV */
    if (opcode == 0x88) {
        /* MOV r/m8, r8 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x89) {
        /* MOV r/m16/32/64, r16/32/64 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x8A) {
        /* MOV r8, r/m8 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x8B) {
        /* MOV r16/32/64, r/m16/32/64 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x8D) {
        /* LEA r32/64, m */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_LEA;
        out->writes_flags = false;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
    }
    if (opcode >= 0xB0 && opcode <= 0xB7) {
        /* MOV r8, imm8 */
        uint8_t reg_offset = 0;
        int reg = reg8_idx(opcode & 7, out->has_rex, rex_b, &reg_offset);
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t imm = read_s8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        set_reg_ex(out, 1, reg, 1, reg_offset);
        set_imm(out, 2, imm, 1);
        return HB_OK;
    }
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        /* MOV r16/32/64, imm16/32/64 */
        int reg = reg_idx(opcode & 7, rex_b);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        if (rex_w) {
            if (!can_read(d, 8)) return HB_ERR_DECODE_FAILED;
            set_reg(out, 1, reg, 8);
            set_imm(out, 2, (int64_t)read_u64(d), 8);
        } else if (operand16) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            set_reg(out, 1, reg, 2);
            set_imm(out, 2, (int64_t)read_s16(d), 2);
        } else {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_reg(out, 1, reg, 4);
            set_imm(out, 2, (int64_t)read_s32(d), 4);
        }
        return HB_OK;
    }
    if (opcode == 0xC6) {
        /* MOV r/m8, imm8 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_s8(d), 1);
        refresh_rip_targets(d, out);
        return HB_OK;
    }
    if (opcode == 0xC7) {
        /* MOV r/m16/32/64, imm16/32/64 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        uint8_t sz = rex_w ? 8 : (operand16 ? 2 : 4);
        hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
        if (r != HB_OK) return r;
        if (rex_w) {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s32(d), sz); /* sign-extended 32->64 */
        } else if (operand16) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s16(d), sz);
        } else {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s32(d), sz);
        }
        refresh_rip_targets(d, out);
        return HB_OK;
    }

    /* Group: ADD */
    if (opcode == 0x00) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x01) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x02) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x03) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x04) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x05) {
        out->opcode = HB_INS_ADD; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Group: ADC */
    if (opcode == 0x10) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x11) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x12) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x13) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x14) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x15) {
        out->opcode = HB_INS_ADC; out->writes_flags = true; out->reads_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Group: SUB */
    if (opcode == 0x28) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x29) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x2A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x2B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x2C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x2D) {
        out->opcode = HB_INS_SUB; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Group: SBB */
    if (opcode == 0x18) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x19) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x1A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x1B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x1C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x1D) {
        out->opcode = HB_INS_SBB; out->writes_flags = true; out->reads_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Group: CMP */
    if (opcode == 0x38) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x39) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x3A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x3B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x3C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x3D) {
        out->opcode = HB_INS_CMP; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Group: PUSH / POP */
    if (opcode >= 0x50 && opcode <= 0x57) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -8;
        set_reg(out, 1, reg_idx(opcode & 7, rex_b), 8);
        return HB_OK;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        out->opcode = HB_INS_POP;
        out->stack_delta = 8;
        set_reg(out, 1, reg_idx(opcode & 7, rex_b), 8);
        return HB_OK;
    }
    if (opcode == 0x68) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -8;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 1, (int64_t)read_s32(d), 4);
        return HB_OK;
    }
    if (opcode == 0x6A) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -8;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 1, (int64_t)read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x69 || opcode == 0x6B) {
        /* IMUL r32/64, r/m32/64, imm32/imm8 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_IMUL;
        out->writes_flags = true;
        hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
        if (r != HB_OK) return r;
        if (opcode == 0x69) {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, (int64_t)read_s32(d), out->op1.size);
        } else {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, (int64_t)read_s8(d), out->op1.size);
        }
        refresh_rip_targets(d, out);
        return HB_OK;
    }
    if (opcode == 0x63) {
        /* MOVSXD r64, r/m32. In long mode the source remains 32-bit even
           when REX.W promotes the destination to 64-bit. */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_MOVSXD;
        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
        if (r != HB_OK) return r;
        out->op1.size = rex_w ? 8 : 4;
        out->op2.size = 4;
        return HB_OK;
    }
    if (opcode == 0x98) {
        /* CBW/CWDE/CDQE.  Wine x64 startup code commonly emits REX.W 98
           (cdqe) after int-returning helpers such as wcslen. */
        out->opcode = HB_INS_CDQE;
        if (rex_w) {
            set_reg(out, 1, HB_REG_RAX, 8);
            set_reg(out, 2, HB_REG_RAX, 4);
        } else if (operand16) {
            set_reg(out, 1, HB_REG_RAX, 2);
            set_reg(out, 2, HB_REG_RAX, 1);
        } else {
            set_reg(out, 1, HB_REG_RAX, 4);
            set_reg(out, 2, HB_REG_RAX, 2);
        }
        return HB_OK;
    }
    if (opcode == 0x99) {
        /* CWD/CDQ/CQO: sign-extend AX/EAX/RAX into DX:AX/EDX:EAX/RDX:RAX. */
        out->opcode = HB_INS_CWD;
        if (rex_w) set_reg(out, 1, HB_REG_RAX, 8);
        else if (operand16) set_reg(out, 1, HB_REG_RAX, 2);
        else set_reg(out, 1, HB_REG_RAX, 4);
        return HB_OK;
    }
    if (opcode == 0xAE || opcode == 0xAF) {
        /* SCAS m8/m16/m32/m64. REPNE/REPE are carried as an immediate mode. */
        out->opcode = HB_INS_SCAS;
        uint8_t sz = (opcode == 0xAE) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        set_reg(out, 1, HB_REG_RAX, sz);
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAA || opcode == 0xAB) {
        /* STOS m8/m16/m32/m64. REP is carried as an immediate mode. */
        out->opcode = HB_INS_STOS;
        uint8_t sz = (opcode == 0xAA) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        set_reg(out, 1, HB_REG_RAX, sz);
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
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
        out->stack_delta = -8;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        int32_t rel = read_s32(d);
        out->branch_target = addr + d->pos + rel;
        return HB_OK;
    }

    /* Group: RET */
    if (opcode == 0xC3) {
        out->opcode = HB_INS_RET;
        out->is_ret = true;
        out->stack_delta = 8;
        return HB_OK;
    }
    if (opcode == 0xC2) {
        out->opcode = HB_INS_RET;
        out->is_ret = true;
        out->stack_delta = 8;
        if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
        uint16_t imm = (uint16_t)(read_u8(d) | (read_u8(d) << 8));
        out->ret_imm = imm;
        return HB_OK;
    }

    if (opcode == 0x9F) {
        out->opcode = HB_INS_LAHF;
        out->reads_flags = true;
        return HB_OK;
    }

    if (opcode == 0x9E) {
        out->opcode = HB_INS_SAHF;
        out->writes_flags = true;
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
    if (opcode == 0x86 || opcode == 0x87) {
        /* XCHG r/m, r. LOCK is valid for memory operands and is consumed by
           the prefix scanner; execution stays atomic at the lifted op level. */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XCHG;
        uint8_t def_size = (opcode == 0x86) ? 1 : 4;
        hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, def_size, out, 1, 2, true);
        if (r != HB_OK) return r;
        return HB_OK;
    }

    /* Two-byte opcode: 0x0F ... */
    if (opcode == 0x0F) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (op2 == 0xA2) {
            out->opcode = HB_INS_CPUID;
            return HB_OK;
        }
        if (op2 == 0x01) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (modrm == 0xD0) {
                out->opcode = HB_INS_XGETBV;
                return HB_OK;
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
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
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 >= 0x40 && op2 <= 0x4F) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CMOVcc;
            out->cond = cond_from_cc(op2 & 0x0F);
            out->reads_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xBA) {
            /* Group 8: BT/BTS/BTR/BTC r/m16/32/64, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if (ext == 4) out->opcode = HB_INS_BT;
            else if (ext == 5) out->opcode = HB_INS_BTS;
            else if (ext == 6) out->opcode = HB_INS_BTR;
            else if (ext == 7) out->opcode = HB_INS_BTC;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, rex_w ? 8 : (operand16 ? 2 : 4), out, 1);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_u8(d), 1);
            refresh_rip_targets(d, out);
            return HB_OK;
        }
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            /* BT/BTS/BTR/BTC r/m16/32/64, r16/32/64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xA3) out->opcode = HB_INS_BT;
            else if (op2 == 0xAB) out->opcode = HB_INS_BTS;
            else if (op2 == 0xB3) out->opcode = HB_INS_BTR;
            else out->opcode = HB_INS_BTC;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                        operand16 ? 2 : 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0x1F) {
            /* Multi-byte NOP: 0F 1F /r, often padded with 66/2E prefixes. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_NOP;
            hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, 1, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0x0D || op2 == 0x18) {
            /* PREFETCH/PREFETCHW/PREFETCHT* r/m8 groups are cache hints; execute as NOP. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_NOP;
            hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, 1, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xAF) {
            /* IMUL r32/64, r/m32/64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_IMUL;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x2A) {
            /* CVTSI2SD xmm, r/m32 or r/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTSI2SD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, rex_w ? 8 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x2A) {
            /* CVTSI2SS xmm, r/m32 or r/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTSI2SS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, rex_w ? 8 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            return HB_OK;
        }
        if (operand16 && op2 == 0x6E) {
            /* MOVD/MOVQ xmm, r/m32/r/m64. Win7 Calc uses MOVD before CVTDQ2PD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            out->op2.size = rex_w ? 8 : 4;
            return HB_OK;
        }
        if (operand16 && op2 == 0x7E) {
            /* MOVD/MOVQ r/m32/r/m64, xmm. Notepad++ uses REX.W MOVQ to pass qword packs. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            out->op1.size = rex_w ? 8 : 4;
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && op2 == 0xD6) {
            /* MOVQ xmm/m64, xmm. Same transfer family as MOVD/MOVQ 6E/7E, but
             * always stores/copies the low qword from the source XMM operand. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, true);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op1.is_mem) out->op1.size = 8;
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x7E) {
            /* MOVQ xmm, xmm/m64. Completes the SSE qword transfer decode family
             * used by compiler-generated helper code around GDI toolbar probes. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0xE6) {
            /* CVTDQ2PD xmm, xmm/m64: convert two signed dwords to two doubles. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTDQ2PD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if ((!operand16 && !prefix_f2 && !prefix_f3 && op2 == 0x5B) ||
            (operand16 && op2 == 0x5B) ||
            (prefix_f3 && op2 == 0x5B)) {
            /* Packed conversion family:
             *   0F 5B       CVTDQ2PS xmm, xmm/m128
             *   66 0F 5B    CVTPS2DQ xmm, xmm/m128
             *   F3 0F 5B    CVTTPS2DQ xmm, xmm/m128
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (prefix_f3) out->opcode = HB_INS_CVTTPS2DQ;
            else if (operand16) out->opcode = HB_INS_CVTPS2DQ;
            else out->opcode = HB_INS_CVTDQ2PS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x5A) {
            /* SSE convert family:
             *   0F 5A       CVTPS2PD xmm, xmm/m64
             *   66 0F 5A    CVTPD2PS xmm, xmm/m128
             *   F3 0F 5A    CVTSS2SD xmm, xmm/m32
             *   F2 0F 5A    CVTSD2SS xmm, xmm/m64
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 8;
            if (prefix_f2) out->opcode = HB_INS_CVTSD2SS;
            else if (prefix_f3) { out->opcode = HB_INS_CVTSS2SD; mem_size = 4; }
            else if (operand16) { out->opcode = HB_INS_CVTPD2PS; mem_size = 16; }
            else out->opcode = HB_INS_CVTPS2PD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x5E) {
            /* DIVSD xmm, xmm/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_DIVSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (op2 == 0x58 || op2 == 0x5C) {
            /* Floating ADD/SUB family:
             *   0F 58/5C       ADDPS/SUBPS xmm, xmm/m128
             *   66 0F 58/5C    ADDPD/SUBPD xmm, xmm/m128
             *   F3 0F 58/5C    ADDSS/SUBSS xmm, xmm/m32
             *   F2 0F 58/5C    ADDSD/SUBSD xmm, xmm/m64
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 16;
            if (prefix_f2) {
                out->opcode = (op2 == 0x58) ? HB_INS_ADDSD : HB_INS_SUBSD;
                mem_size = 8;
            } else if (prefix_f3) {
                out->opcode = (op2 == 0x58) ? HB_INS_ADDSS : HB_INS_SUBSS;
                mem_size = 4;
            } else if (operand16) {
                out->opcode = (op2 == 0x58) ? HB_INS_ADDPD : HB_INS_SUBPD;
            } else {
                out->opcode = (op2 == 0x58) ? HB_INS_ADDPS : HB_INS_SUBPS;
            }
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x59) {
            /* MULSD xmm, xmm/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MULSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (prefix_f3 && (op2 == 0x5E || op2 == 0x59)) {
            /* DIVSS/MULSS xmm, xmm/m32 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0x5E) ? HB_INS_DIVSS : HB_INS_MULSS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 4;
            return HB_OK;
        }
        if (op2 == 0x5D || op2 == 0x5F) {
            /* Floating MIN/MAX family:
             *   0F 5D/5F       MINPS/MAXPS xmm, xmm/m128
             *   66 0F 5D/5F    MINPD/MAXPD xmm, xmm/m128
             *   F3 0F 5D/5F    MINSS/MAXSS xmm, xmm/m32
             *   F2 0F 5D/5F    MINSD/MAXSD xmm, xmm/m64
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 16;
            if (prefix_f2) { out->opcode = (op2 == 0x5D) ? HB_INS_MINSD : HB_INS_MAXSD; mem_size = 8; }
            else if (prefix_f3) { out->opcode = (op2 == 0x5D) ? HB_INS_MINSS : HB_INS_MAXSS; mem_size = 4; }
            else if (operand16) out->opcode = (op2 == 0x5D) ? HB_INS_MINPD : HB_INS_MAXPD;
            else out->opcode = (op2 == 0x5D) ? HB_INS_MINPS : HB_INS_MAXPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if ((op2 == 0x2E || op2 == 0x2F) && !prefix_f2 && !prefix_f3) {
            /* COMISS/UCOMISS and COMISD/UCOMISD. We model exception differences
             * conservatively and share ordered flag semantics for both pairs. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_COMISD : HB_INS_COMISS;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                        operand16 ? 8 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = operand16 ? 8 : 4;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x2C) {
            /* CVTTSD2SI r32/r64, xmm/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTTSD2SI;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = rex_w ? 8 : 4;
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x2C) {
            /* CVTTSS2SI r32/r64, xmm/m32 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTTSS2SI;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = rex_w ? 8 : 4;
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 4;
            return HB_OK;
        }
        if ((!operand16 && !prefix_f2 && !prefix_f3 && op2 >= 0x54 && op2 <= 0x57) ||
            (operand16 && op2 >= 0x54 && op2 <= 0x57) ||
            (operand16 && (op2 == 0xDB || op2 == 0xDF || op2 == 0xEB || op2 == 0xEF))) {
            /* Packed XMM bitwise logical family: AND/ANDN/OR/XOR, PS/PD and integer forms. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0x54 || op2 == 0xDB) out->opcode = HB_INS_XMM_AND;
            else if (op2 == 0x55 || op2 == 0xDF) out->opcode = HB_INS_XMM_ANDN;
            else if (op2 == 0x56 || op2 == 0xEB) out->opcode = HB_INS_XMM_OR;
            else out->opcode = (operand16 && op2 == 0xEF) ? HB_INS_PXOR : HB_INS_XORPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && (op2 == 0x64 || op2 == 0x65 || op2 == 0x66 ||
                          op2 == 0x74 || op2 == 0x75 || op2 == 0x76)) {
            /* PCMPGTB/W/D and PCMPEQB/W/D xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0x64) out->opcode = HB_INS_PCMPGTB;
            else if (op2 == 0x65) out->opcode = HB_INS_PCMPGTW;
            else if (op2 == 0x66) out->opcode = HB_INS_PCMPGTD;
            else if (op2 == 0x74) out->opcode = HB_INS_PCMPEQB;
            else if (op2 == 0x75) out->opcode = HB_INS_PCMPEQW;
            else out->opcode = HB_INS_PCMPEQD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if ((op2 == 0x14 || op2 == 0x15) && !prefix_f2 && !prefix_f3) {
            /* UNPCKLPS/UNPCKLPD/UNPCKHPS/UNPCKHPD, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0x14) out->opcode = operand16 ? HB_INS_UNPCKLPD : HB_INS_UNPCKLPS;
            else out->opcode = operand16 ? HB_INS_UNPCKHPD : HB_INS_UNPCKHPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && op2 == 0xD7) {
            /* PMOVMSKB r32, xmm: extract byte sign bits into a zero-extended mask. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_PMOVMSKB;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 2);
            out->op1.size = 4;
            return HB_OK;
        }
        if ((!operand16 && !prefix_f2 && !prefix_f3 && op2 == 0x50) ||
            (operand16 && op2 == 0x50)) {
            /* MOVMSKPS/MOVMSKPD r32, xmm: extract packed FP sign bits. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_MOVMSKPD : HB_INS_MOVMSKPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (!out->op2.is_reg) return HB_ERR_UNSUPPORTED_OPCODE;
            mark_xmm_operand(out, 2);
            out->op1.size = 4;
            return HB_OK;
        }
        if (operand16 && ((op2 >= 0x60 && op2 <= 0x62) ||
                          (op2 >= 0x68 && op2 <= 0x6A) ||
                          op2 == 0x6C || op2 == 0x6D)) {
            /* SSE2 PUNPCK low/high integer unpack family, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0x60) out->opcode = HB_INS_PUNPCKLBW;
            else if (op2 == 0x61) out->opcode = HB_INS_PUNPCKLWD;
            else if (op2 == 0x62) out->opcode = HB_INS_PUNPCKLDQ;
            else if (op2 == 0x6C) out->opcode = HB_INS_PUNPCKLQDQ;
            else if (op2 == 0x68) out->opcode = HB_INS_PUNPCKHBW;
            else if (op2 == 0x69) out->opcode = HB_INS_PUNPCKHWD;
            else if (op2 == 0x6A) out->opcode = HB_INS_PUNPCKHDQ;
            else out->opcode = HB_INS_PUNPCKHQDQ;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if ((operand16 && op2 == 0x70) || (prefix_f2 && op2 == 0x70) ||
            (prefix_f3 && op2 == 0x70)) {
            /* PSHUFD/PSHUFLW/PSHUFHW xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (operand16) out->opcode = HB_INS_PSHUFD;
            else if (prefix_f2) out->opcode = HB_INS_PSHUFLW;
            else out->opcode = HB_INS_PSHUFHW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            return HB_OK;
        }
        if (operand16 && (op2 == 0x71 || op2 == 0x72 || op2 == 0x73)) {
            /* SSE2 XMM immediate shifts. 0F 71/72 use /2,/4,/6; 0F 73 also has byte shifts. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if (op2 == 0x71 && ext == 2) out->opcode = HB_INS_PSRLW;
            else if (op2 == 0x71 && ext == 4) out->opcode = HB_INS_PSRAW;
            else if (op2 == 0x71 && ext == 6) out->opcode = HB_INS_PSLLW;
            else if (op2 == 0x72 && ext == 2) out->opcode = HB_INS_PSRLD;
            else if (op2 == 0x72 && ext == 4) out->opcode = HB_INS_PSRAD;
            else if (op2 == 0x72 && ext == 6) out->opcode = HB_INS_PSLLD;
            else if (op2 == 0x73 && ext == 2) out->opcode = HB_INS_PSRLQ;
            else if (op2 == 0x73 && ext == 3) out->opcode = HB_INS_PSRLDQ;
            else if (op2 == 0x73 && ext == 6) out->opcode = HB_INS_PSLLQ;
            else if (op2 == 0x73 && ext == 7) out->opcode = HB_INS_PSLLDQ;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            out->writes_flags = false;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 16, out, 1);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_u8(d), 1);
            return HB_OK;
        }
        if (operand16 && (op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA || op2 == 0xFB)) {
            /* Packed integer subtract family: PSUBB/PSUBW/PSUBD/PSUBQ xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xF8) out->opcode = HB_INS_PSUBB;
            else if (op2 == 0xF9) out->opcode = HB_INS_PSUBW;
            else if (op2 == 0xFA) out->opcode = HB_INS_PSUBD;
            else out->opcode = HB_INS_PSUBQ;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && (op2 == 0xFC || op2 == 0xFD || op2 == 0xFE || op2 == 0xD4)) {
            /* Packed integer add family: PADDB/PADDW/PADDD/PADDQ xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xFC) out->opcode = HB_INS_PADDB;
            else if (op2 == 0xFD) out->opcode = HB_INS_PADDW;
            else if (op2 == 0xFE) out->opcode = HB_INS_PADDD;
            else out->opcode = HB_INS_PADDQ;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0xBC) {
            /* TZCNT/BSF r32/64, r/m32/64. We normalize this path to TZCNT;
               Wine uses the F3-prefixed form in ntdll's wait primitives. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_TZCNT;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xBD) {
            /* BSR r32/64, r/m32/64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_BSR;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            /* BSWAP r32/r64. REX.B extends the opcode register field. */
            out->opcode = HB_INS_BSWAP;
            out->writes_flags = false;
            set_reg(out, 1, reg_idx(op2 - 0xC8, rex_b), rex_w ? 8 : 4);
            return HB_OK;
        }
        if (op2 == 0xB6 || op2 == 0xB7) {
            /* MOVZX r32/64, r/m8 or r/m16 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVZX;
            out->writes_flags = false;
            uint8_t dst_size = rex_w ? 8 : 4;
            uint8_t src_size = (op2 == 0xB6) ? 1 : 2;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, dst_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = dst_size;
            out->op2.size = src_size;
            return HB_OK;
        }
        if (op2 == 0xBE || op2 == 0xBF) {
            /* MOVSX r32/64, r/m8 or r/m16 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVSX;
            out->writes_flags = false;
            uint8_t dst_size = rex_w ? 8 : 4;
            uint8_t src_size = (op2 == 0xBE) ? 1 : 2;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, dst_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = dst_size;
            out->op2.size = src_size;
            return HB_OK;
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
            op2 == 0x6F || op2 == 0x7F) {
            /* MOVUPS/MOVAPS/MOVDQA/MOVDQU are 128-bit moves.  Legacy scalar
             * MOVSS/MOVSD (F3/F2 0F 10/11) only transfers the low 4/8 bytes;
             * the interpreter preserves the rest of the destination XMM reg. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            bool scalar_move = (op2 == 0x10 || op2 == 0x11) && (prefix_f2 || prefix_f3);
            uint8_t move_size = scalar_move ? (prefix_f3 ? 4 : 8) : 16;
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, move_size, out, 1, 2,
                                        (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F));
            if (r != HB_OK) return r;
            mark_xmm_operands(out);
            if (scalar_move) {
                if (out->op1.is_reg) out->op1.size = move_size;
                if (out->op2.is_reg) out->op2.size = move_size;
                if (out->op1.is_mem) out->op1.size = move_size;
                if (out->op2.is_mem) out->op2.size = move_size;
            }
            return HB_OK;
        }
        if (op2 == 0xC0 || op2 == 0xC1) {
            /* XADD r/m, r. LOCK is valid for memory operands and is consumed
             * as a prefix; the interpreter executes this as one atomic IR op.
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_XADD;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                        op2 == 0xC0 ? 1 : 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xB0 || op2 == 0xB1) {
            /* CMPXCHG r/m8,r8 and r/m32/64,r32/64. LOCK is consumed as a prefix above;
               the interpreter is currently single-threaded at the guest block
               level, so atomicity is provided by staying inside this operation. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CMPXCHG;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                        op2 == 0xB0 ? 1 : 4, out, 1, 2, true);
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
        uint8_t sz = (opcode == 0x80) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));

        if (ext == 0) out->opcode = HB_INS_ADD;
        else if (ext == 1) out->opcode = HB_INS_OR;
        else if (ext == 2) { out->opcode = HB_INS_ADC; out->reads_flags = true; }
        else if (ext == 3) { out->opcode = HB_INS_SBB; out->reads_flags = true; }
        else if (ext == 4) out->opcode = HB_INS_AND;
        else if (ext == 5) out->opcode = HB_INS_SUB;
        else if (ext == 6) out->opcode = HB_INS_XOR;
        else if (ext == 7) out->opcode = HB_INS_CMP;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;

        hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz == 1 ? 1 : sz, out, 1);
        if (r != HB_OK) return r;

        if (opcode == 0x80) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_s8(d), 1);
        } else if (opcode == 0x81) {
            if (operand16 && !rex_w) {
                if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s16(d), sz);
            } else {
                if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s32(d), sz);
            }
        } else { /* 0x83 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s8(d), sz);
        }
        refresh_rip_targets(d, out);
        return HB_OK;
    }

    /* Group: ROL/ROR/SHL/SHR/SAR by imm8 (C0/C1) */
    if (opcode == 0xC0 || opcode == 0xC1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) out->opcode = HB_INS_ROL;
        else if (ext == 1) out->opcode = HB_INS_ROR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        uint8_t sz = (opcode == 0xC0) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_u8(d) & (rex_w ? 0x3F : 0x1F), 1);
        refresh_rip_targets(d, out);
        return HB_OK;
    }

    /* Group: ROL/ROR/SHL/SHR/SAR by 1 (D0/D1) */
    if (opcode == 0xD0 || opcode == 0xD1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) out->opcode = HB_INS_ROL;
        else if (ext == 1) out->opcode = HB_INS_ROR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        uint8_t sz = (opcode == 0xD0) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
        if (r != HB_OK) return r;
        set_imm(out, 2, 1, 1);
        return HB_OK;
    }

    /* Group: ROL/ROR/SHL/SHR/SAR by CL (D2/D3) */
    if (opcode == 0xD2 || opcode == 0xD3) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) out->opcode = HB_INS_ROL;
        else if (ext == 1) out->opcode = HB_INS_ROR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 7) out->opcode = HB_INS_SAR;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        uint8_t sz = (opcode == 0xD2) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
        if (r != HB_OK) return r;
        set_reg(out, 2, HB_REG_RCX, 1); /* CL */
        return HB_OK;
    }

    /* Group: TEST / NOT / NEG / MUL / IMUL / DIV / IDIV (F6/F7). */
    if (opcode == 0xF6) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) {
            out->opcode = HB_INS_TEST;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_s8(d), 1);
            refresh_rip_targets(d, out);
            return HB_OK;
        }
        if (ext == 2) out->opcode = HB_INS_NOT;
        else if (ext == 3) out->opcode = HB_INS_NEG;
        else if (ext == 4) out->opcode = HB_INS_MUL;
        else if (ext == 5) out->opcode = HB_INS_IMUL;
        else if (ext == 6) out->opcode = HB_INS_DIV;
        else if (ext == 7) out->opcode = HB_INS_IDIV;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = (ext == 3 || ext == 4 || ext == 5);
        hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        if (r != HB_OK) return r;
        return HB_OK;
    }
    if (opcode == 0xF7) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        uint8_t sz = rex_w ? 8 : (operand16 ? 2 : 4);
        if (ext == 0) {
            out->opcode = HB_INS_TEST;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
            if (r != HB_OK) return r;
            if (operand16 && !rex_w) {
                if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s16(d), out->op1.size);
            } else {
                if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s32(d), out->op1.size);
            }
            refresh_rip_targets(d, out);
            return HB_OK;
        }
        if (ext == 2) out->opcode = HB_INS_NOT;
        else if (ext == 3) out->opcode = HB_INS_NEG;
        else if (ext == 4) out->opcode = HB_INS_MUL;
        else if (ext == 5) out->opcode = HB_INS_IMUL;
        else if (ext == 6) out->opcode = HB_INS_DIV;
        else if (ext == 7) out->opcode = HB_INS_IDIV;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = (ext == 3 || ext == 4 || ext == 5);
        return parse_modrm_ext(d, modrm, rex_w, rex_b, sz, out, 1);
    }

    /* Group: 0xFF */
    if (opcode == 0xFF) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        uint8_t incdec_size = rex_w ? 8 : (operand16 ? 2 : 4);
        if (ext == 0) {
            /* INC r/m16/32/64.  Operand size follows 0x66/REX.W. */
            out->opcode = HB_INS_INC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, incdec_size, out, 1);
        }
        if (ext == 1) {
            /* DEC r/m16/32/64.  Operand size follows 0x66/REX.W. */
            out->opcode = HB_INS_DEC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, incdec_size, out, 1);
        }
        if (ext == 2) {
            /* CALL r/m */
            out->opcode = HB_INS_CALL;
            out->is_call = true;
            out->stack_delta = -8;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 8, out, 1);
        }
        if (ext == 4) {
            /* JMP r/m */
            out->opcode = HB_INS_JMP;
            out->is_branch = true;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 8, out, 1);
        }
        if (ext == 6) {
            /* PUSH r/m */
            out->opcode = HB_INS_PUSH;
            out->stack_delta = -8;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 8, out, 1);
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    /* Group: 0xFE byte INC/DEC */
    if (opcode == 0xFE) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) {
            out->opcode = HB_INS_INC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        }
        if (ext == 1) {
            out->opcode = HB_INS_DEC;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    /* TEST (subset) */
    if (opcode == 0x84) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x85) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, true);
    }
    if (opcode == 0xA8) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0xA9) {
        out->opcode = HB_INS_TEST; out->writes_flags = true;
        uint8_t sz = rex_w ? 8 : 4;
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, HB_REG_RAX, sz);
        set_imm(out, 2, (int64_t)read_s32(d), sz);
        return HB_OK;
    }

    /* AND (subset) */
    if (opcode == 0x20) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x21) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x22) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x23) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x24) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_AND; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x25) {
        out->opcode = HB_INS_AND; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* OR (subset) */
    if (opcode == 0x08) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x09) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x0A) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x0B) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x0C) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_OR; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x0D) {
        out->opcode = HB_INS_OR; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* XOR (subset) */
    if (opcode == 0x30) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, true);
    }
    if (opcode == 0x31) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
    }
    if (opcode == 0x32) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 1, out, 1, 2, false);
    }
    if (opcode == 0x33) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, false);
    }
    if (opcode == 0x34) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0x35) {
        out->opcode = HB_INS_XOR; out->writes_flags = true;
        return set_acc_imm_op(d, out, rex_w, operand16);
    }

    /* Unsupported */
    return HB_ERR_UNSUPPORTED_OPCODE;
}

hb_result_t hb_decode_x64(const uint8_t* code, size_t len, uint64_t addr, hb_decoded_t* out) {
    if (!code || !out || len == 0) return HB_ERR_INVALID_ARG;

    hb_dec_t d = {
        .code = code,
        .len = len,
        .pos = 0,
        .addr = addr,
        .fault = false
    };
    hb_result_t r = decode_one(&d, out);
    if (d.fault && r == HB_OK) r = HB_ERR_DECODE_FAILED;
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

hb_decoder_t* hb_decoder_create(hb_arch_t arch, const uint8_t* code, size_t code_len, uint64_t base_addr) {
    (void)arch;
    hb_decoder_t* d = (hb_decoder_t*)calloc(1, sizeof(hb_decoder_t));
    if (!d) return NULL;
    d->arch = arch;
    d->code = code;
    d->code_len = code_len;
    d->base_addr = base_addr;
    d->pos = 0;
    return d;
}

void hb_decoder_destroy(hb_decoder_t* d) {
    free(d);
}

hb_result_t hb_decode_next(hb_decoder_t* d, hb_decoded_t* out) {
    if (!d || !out) return HB_ERR_INVALID_ARG;
    if (d->pos >= d->code_len) return HB_ERR_DECODE_FAILED;
    hb_result_t r = hb_decode_x64(d->code + d->pos, d->code_len - d->pos,
                                    d->base_addr + d->pos, out);
    if (r == HB_OK || r == HB_ERR_UNSUPPORTED_OPCODE) {
        d->pos += out->len;
    } else if (r == HB_ERR_DECODE_FAILED) {
        d->pos += out->len > 0 ? out->len : 1;
    }
    return r;
}

hb_result_t hb_decode_at(hb_decoder_t* d, size_t offset, hb_decoded_t* out) {
    if (!d || !out || offset >= d->code_len) return HB_ERR_INVALID_ARG;
    return hb_decode_x64(d->code + offset, d->code_len - offset,
                         d->base_addr + offset, out);
}

int hb_reg_index_from_modrm(uint8_t modrm, bool rex_r, bool is_64bit) {
    (void)is_64bit;
    return reg_idx((modrm >> 3) & 7, rex_r);
}

int hb_rm_index_from_modrm(uint8_t modrm, bool rex_b, bool is_64bit) {
    (void)is_64bit;
    return reg_idx(modrm & 7, rex_b);
}

const char* hb_opcode_name(int opcode) {
    switch (opcode) {
        case HB_INS_MOV: return "MOV";
        case HB_INS_LEA: return "LEA";
        case HB_INS_ADD: return "ADD";
        case HB_INS_ADC: return "ADC";
        case HB_INS_SUB: return "SUB";
        case HB_INS_SBB: return "SBB";
        case HB_INS_AND: return "AND";
        case HB_INS_OR: return "OR";
        case HB_INS_XOR: return "XOR";
        case HB_INS_NOT: return "NOT";
        case HB_INS_NEG: return "NEG";
        case HB_INS_INC: return "INC";
        case HB_INS_DEC: return "DEC";
        case HB_INS_MUL: return "MUL";
        case HB_INS_IMUL: return "IMUL";
        case HB_INS_DIV: return "DIV";
        case HB_INS_IDIV: return "IDIV";
        case HB_INS_BT: return "BT";
        case HB_INS_BTS: return "BTS";
        case HB_INS_BTR: return "BTR";
        case HB_INS_BTC: return "BTC";
        case HB_INS_SHL: return "SHL";
        case HB_INS_SHR: return "SHR";
        case HB_INS_SAR: return "SAR";
        case HB_INS_ROL: return "ROL";
        case HB_INS_ROR: return "ROR";
        case HB_INS_CMP: return "CMP";
        case HB_INS_TEST: return "TEST";
        case HB_INS_CMPXCHG: return "CMPXCHG";
        case HB_INS_XCHG: return "XCHG";
        case HB_INS_XADD: return "XADD";
        case HB_INS_PUSH: return "PUSH";
        case HB_INS_POP: return "POP";
        case HB_INS_CALL: return "CALL";
        case HB_INS_RET: return "RET";
        case HB_INS_JMP: return "JMP";
        case HB_INS_Jcc: return "Jcc";
        case HB_INS_SETcc: return "SETcc";
        case HB_INS_CMOVcc: return "CMOVcc";
        case HB_INS_MOVZX: return "MOVZX";
        case HB_INS_MOVSX: return "MOVSX";
        case HB_INS_MOVSXD: return "MOVSXD";
        case HB_INS_CDQE: return "CDQE";
        case HB_INS_CWD: return "CWD";
        case HB_INS_LEAVE: return "LEAVE";
        case HB_INS_LAHF: return "LAHF";
        case HB_INS_SAHF: return "SAHF";
        case HB_INS_CPUID: return "CPUID";
        case HB_INS_XGETBV: return "XGETBV";
        case HB_INS_NOP: return "NOP";
        case HB_INS_SCAS: return "SCAS";
        case HB_INS_STOS: return "STOS";
        case HB_INS_TZCNT: return "TZCNT";
        case HB_INS_BSR: return "BSR";
        case HB_INS_BSWAP: return "BSWAP";
        case HB_INS_SSE_MOV: return "SSE_MOV";
        case HB_INS_XMM_AND: return "XMM_AND";
        case HB_INS_XMM_ANDN: return "XMM_ANDN";
        case HB_INS_XMM_OR: return "XMM_OR";
        case HB_INS_XORPS: return "XORPS";
        case HB_INS_PXOR: return "PXOR";
        case HB_INS_PCMPEQB: return "PCMPEQB";
        case HB_INS_PCMPEQW: return "PCMPEQW";
        case HB_INS_PCMPEQD: return "PCMPEQD";
        case HB_INS_PCMPGTB: return "PCMPGTB";
        case HB_INS_PCMPGTW: return "PCMPGTW";
        case HB_INS_PCMPGTD: return "PCMPGTD";
        case HB_INS_PMOVMSKB: return "PMOVMSKB";
        case HB_INS_MOVMSKPS: return "MOVMSKPS";
        case HB_INS_MOVMSKPD: return "MOVMSKPD";
        case HB_INS_UNPCKLPS: return "UNPCKLPS";
        case HB_INS_UNPCKLPD: return "UNPCKLPD";
        case HB_INS_UNPCKHPS: return "UNPCKHPS";
        case HB_INS_UNPCKHPD: return "UNPCKHPD";
        case HB_INS_PUNPCKLBW: return "PUNPCKLBW";
        case HB_INS_PUNPCKLWD: return "PUNPCKLWD";
        case HB_INS_PUNPCKLDQ: return "PUNPCKLDQ";
        case HB_INS_PUNPCKLQDQ: return "PUNPCKLQDQ";
        case HB_INS_PUNPCKHBW: return "PUNPCKHBW";
        case HB_INS_PUNPCKHWD: return "PUNPCKHWD";
        case HB_INS_PUNPCKHDQ: return "PUNPCKHDQ";
        case HB_INS_PUNPCKHQDQ: return "PUNPCKHQDQ";
        case HB_INS_PSHUFD: return "PSHUFD";
        case HB_INS_PSHUFLW: return "PSHUFLW";
        case HB_INS_PSHUFHW: return "PSHUFHW";
        case HB_INS_PSRLW: return "PSRLW";
        case HB_INS_PSRAW: return "PSRAW";
        case HB_INS_PSLLW: return "PSLLW";
        case HB_INS_PSRLD: return "PSRLD";
        case HB_INS_PSRAD: return "PSRAD";
        case HB_INS_PSLLD: return "PSLLD";
        case HB_INS_PSRLQ: return "PSRLQ";
        case HB_INS_PSLLQ: return "PSLLQ";
        case HB_INS_PSRLDQ: return "PSRLDQ";
        case HB_INS_PSLLDQ: return "PSLLDQ";
        case HB_INS_MOVD: return "MOVD";
        case HB_INS_CVTDQ2PD: return "CVTDQ2PD";
        case HB_INS_CVTDQ2PS: return "CVTDQ2PS";
        case HB_INS_CVTPS2DQ: return "CVTPS2DQ";
        case HB_INS_CVTTPS2DQ: return "CVTTPS2DQ";
        case HB_INS_CVTPS2PD: return "CVTPS2PD";
        case HB_INS_CVTPD2PS: return "CVTPD2PS";
        case HB_INS_CVTSS2SD: return "CVTSS2SD";
        case HB_INS_CVTSD2SS: return "CVTSD2SS";
        case HB_INS_CVTSI2SD: return "CVTSI2SD";
        case HB_INS_CVTSI2SS: return "CVTSI2SS";
        case HB_INS_ADDPS: return "ADDPS";
        case HB_INS_ADDPD: return "ADDPD";
        case HB_INS_ADDSS: return "ADDSS";
        case HB_INS_ADDSD: return "ADDSD";
        case HB_INS_SUBPS: return "SUBPS";
        case HB_INS_SUBPD: return "SUBPD";
        case HB_INS_SUBSS: return "SUBSS";
        case HB_INS_SUBSD: return "SUBSD";
        case HB_INS_DIVSD: return "DIVSD";
        case HB_INS_MULSD: return "MULSD";
        case HB_INS_DIVSS: return "DIVSS";
        case HB_INS_MULSS: return "MULSS";
        case HB_INS_MINPS: return "MINPS";
        case HB_INS_MAXPS: return "MAXPS";
        case HB_INS_MINPD: return "MINPD";
        case HB_INS_MAXPD: return "MAXPD";
        case HB_INS_MINSS: return "MINSS";
        case HB_INS_MAXSS: return "MAXSS";
        case HB_INS_MINSD: return "MINSD";
        case HB_INS_MAXSD: return "MAXSD";
        case HB_INS_COMISS: return "COMISS";
        case HB_INS_COMISD: return "COMISD";
        case HB_INS_CVTTSD2SI: return "CVTTSD2SI";
        case HB_INS_CVTTSS2SI: return "CVTTSS2SI";
        case HB_INS_PADDB: return "PADDB";
        case HB_INS_PADDW: return "PADDW";
        case HB_INS_PADDD: return "PADDD";
        case HB_INS_PADDQ: return "PADDQ";
        case HB_INS_PSUBB: return "PSUBB";
        case HB_INS_PSUBW: return "PSUBW";
        case HB_INS_PSUBD: return "PSUBD";
        case HB_INS_PSUBQ: return "PSUBQ";
        case HB_INS_UNKNOWN: return "UNKNOWN";
        case HB_INS_UNSUPPORTED: return "UNSUPPORTED";
        default: return "?";
    }
}

const char* hb_cond_name(int cond) {
    switch (cond) {
        case HB_COND_NONE: return "";
        case HB_COND_E: return "E";
        case HB_COND_NE: return "NE";
        case HB_COND_S: return "S";
        case HB_COND_NS: return "NS";
        case HB_COND_G: return "G";
        case HB_COND_GE: return "GE";
        case HB_COND_L: return "L";
        case HB_COND_LE: return "LE";
        case HB_COND_A: return "A";
        case HB_COND_AE: return "AE";
        case HB_COND_B: return "B";
        case HB_COND_BE: return "BE";
        case HB_COND_O: return "O";
        case HB_COND_NO: return "NO";
        case HB_COND_P: return "P";
        case HB_COND_NP: return "NP";
        case HB_COND_C: return "C";
        case HB_COND_NC: return "NC";
        default: return "?";
    }
}
