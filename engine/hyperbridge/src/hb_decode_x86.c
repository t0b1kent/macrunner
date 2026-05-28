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

static inline int16_t read_s16(hb_dec_t* d) {
    uint16_t lo = (uint16_t)(d->code[d->pos] | (d->code[d->pos+1] << 8u));
    d->pos += 2;
    return (int16_t)lo;
}

static inline int32_t read_s32(hb_dec_t* d) {
    uint32_t lo = d->code[d->pos] | (d->code[d->pos+1] << 8u)
                 | (d->code[d->pos+2] << 16u) | (d->code[d->pos+3] << 24u);
    d->pos += 4;
    return (int32_t)lo;
}

static inline int reg8_idx(int base, uint8_t* byte_offset) {
    if (byte_offset) *byte_offset = 0;
    if (base >= 4 && base <= 7) {
        if (byte_offset) *byte_offset = 1;
        return base - 4; /* AH/CH/DH/BH alias byte 1 of EAX/ECX/EDX/EBX. */
    }
    return base;
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

    uint8_t sz = def_size;
    uint8_t reg_offset = 0;
    int reg = (sz == 1) ? reg8_idx(reg_op, &reg_offset) : reg_op;

    if (mod == 3) {
        uint8_t rm_offset = 0;
        int rm_reg = (sz == 1) ? reg8_idx(rm, &rm_offset) : rm;
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
        set_reg_ex(out, src_slot, reg, sz, reg_offset);
    } else {
        set_reg_ex(out, dst_slot, reg, sz, reg_offset);
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
        uint8_t rm_offset = 0;
        int rm_reg = (sz == 1) ? reg8_idx(rm, &rm_offset) : rm;
        set_reg_ex(out, op_slot, rm_reg, sz, rm_offset);
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

static hb_result_t decode_x87(hb_dec_t* d, uint8_t opcode, hb_decoded_t* out) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_op;
    uint8_t size = 0;

    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    modrm = read_u8(d);
    mod = (modrm >> 6) & 3u;
    reg_op = (modrm >> 3) & 7u;
    out->writes_flags = false;

    if (opcode == 0xdf && modrm == 0xe0) {
        out->opcode = HB_INS_X87_FNSTSW;
        set_reg(out, 1, HB_REG_X86_EAX, 2);
        return HB_OK;
    }
    if (mod == 3) {
        uint8_t sti = modrm & 7u;
        if (opcode == 0xd8) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FADD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FMUL;
            else if (modrm >= 0xd0 && modrm <= 0xd7) out->opcode = HB_INS_X87_FCOM;
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCOMP;
            else if (modrm >= 0xe0 && modrm <= 0xe7) out->opcode = HB_INS_X87_FSUB;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FSUBR;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FDIV;
            else if (modrm >= 0xf8) out->opcode = HB_INS_X87_FDIVR;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xd9) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FLD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FXCH;
            else if (modrm == 0xfc) {
                out->opcode = HB_INS_X87_FRNDINT;
                return HB_OK;
            }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xde) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FADDP;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FMULP;
            else if (modrm == 0xd9) out->opcode = HB_INS_X87_FCOMPP;
            else if (modrm >= 0xe0 && modrm <= 0xe7) out->opcode = HB_INS_X87_FSUBRP;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FSUBP;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FDIVRP;
            else if (modrm >= 0xf8) out->opcode = HB_INS_X87_FDIVP;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xdb) {
            if (modrm == 0xe2) out->opcode = HB_INS_X87_FNCLEX;
            else if (modrm == 0xe3) out->opcode = HB_INS_X87_FNINIT;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    switch (opcode) {
        case 0xd8:
        case 0xdc:
            size = (opcode == 0xd8) ? 4 : 8;
            switch (reg_op) {
                case 0: out->opcode = HB_INS_X87_FADD; break;
                case 1: out->opcode = HB_INS_X87_FMUL; break;
                case 2: out->opcode = HB_INS_X87_FCOM; break;
                case 3: out->opcode = HB_INS_X87_FCOMP; break;
                case 4: out->opcode = HB_INS_X87_FSUB; break;
                case 5: out->opcode = HB_INS_X87_FSUBR; break;
                case 6: out->opcode = HB_INS_X87_FDIV; break;
                case 7: out->opcode = HB_INS_X87_FDIVR; break;
            }
            break;
        case 0xd9:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FLD; size = 4; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 4; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FSTP; size = 4; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FLDCW; size = 2; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FNSTCW; size = 2; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xdb:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 4; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FISTP; size = 4; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xdd:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FLD; size = 8; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 8; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FSTP; size = 8; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xdf:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 2; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FISTP; size = 2; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FILD; size = 8; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FISTP; size = 8; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        default:
            return HB_ERR_UNSUPPORTED_OPCODE;
    }

    return parse_modrm_ext(d, modrm, size, out, 1);
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

    bool operand16 = false;
    bool address16 = false;
    bool prefix_f0 = false;
    bool prefix_f2 = false;
    bool prefix_f3 = false;
    uint8_t opcode = 0;

    while (can_read(d, 1)) {
        uint8_t b = d->code[d->pos];
        if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e ||
            b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67 ||
            b == 0xf0 || b == 0xf2 || b == 0xf3)
        {
            if (b == 0x66) operand16 = true;
            else if (b == 0x67) address16 = true;
            else if (b == 0xf0) prefix_f0 = true;
            else if (b == 0xf2) prefix_f2 = true;
            else if (b == 0xf3) prefix_f3 = true;
            else if (b == 0x64 || b == 0x65) out->segment_prefix = b;
            d->pos++;
        }
        else break;
    }

    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    opcode = read_u8(d);

    /* NOP/cache-hint family. MSVC/Wine use prefixed NOPs (for example
       66 90 at i386 ntdll!LdrInitializeThunk) for alignment. */
    if (opcode == 0x90) {
        out->opcode = HB_INS_NOP;
        return HB_OK;
    }
    if (opcode == 0x9B) {
        /* FWAIT waits for pending x87 exceptions. HyperBridge does not model
           asynchronous x87 exceptions yet, so it is a serialization no-op. */
        out->opcode = HB_INS_NOP;
        return HB_OK;
    }
    if (opcode == 0x86 || opcode == 0x87) {
        /* XCHG r/m8,r8 and r/m16/32,r16/32.  LOCK is consumed by the
           prefix scanner; reject prefixes this decoder cannot model. */
        if (address16 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_XCHG;
        out->writes_flags = false;
        return parse_modrm(d, modrm, opcode == 0x86 ? 1 : (operand16 ? 2 : 4),
                           out, 1, 2, true);
    }
    if (opcode == 0x8C || opcode == 0x8E) {
        /* MOV r/m16,Sreg and MOV Sreg,r/m16.  Segment selector operands stay
           16-bit even with an operand-size prefix. */
        if (address16 || prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t seg = (modrm >> 3) & 7;
        hb_result_t r;
        if (seg > 5) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV_SEG;
        out->writes_flags = false;
        if (opcode == 0x8C) {
            r = parse_modrm_ext(d, modrm, 2, out, 1);
            if (r != HB_OK) return r;
            set_imm(out, 2, seg, 2);
        } else {
            set_imm(out, 1, seg, 2);
            r = parse_modrm_ext(d, modrm, 2, out, 2);
            if (r != HB_OK) return r;
        }
        return HB_OK;
    }
    if (opcode >= 0x91 && opcode <= 0x97) {
        /* XCHG EAX/AX, r32/r16.  0x90 is the EAX,EAX NOP alias above. */
        if (prefix_f0 || address16 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_XCHG;
        out->writes_flags = false;
        set_reg(out, 1, HB_REG_RAX, operand16 ? 2 : 4);
        set_reg(out, 2, opcode & 7, operand16 ? 2 : 4);
        return HB_OK;
    }
    if (opcode == 0x9C || opcode == 0x9D) {
        if (prefix_f0 || address16 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = opcode == 0x9C ? HB_INS_PUSHF : HB_INS_POPF;
        out->writes_flags = opcode == 0x9D;
        out->stack_delta = operand16 ? 2 : 4;
        set_imm(out, 1, 0, operand16 ? 2 : 4);
        return HB_OK;
    }
    if (opcode == 0x9F) {
        if (prefix_f0 || address16 || operand16 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LAHF;
        out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0x9E) {
        if (prefix_f0 || address16 || operand16 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_SAHF;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xA4 || opcode == 0xA5) {
        /* MOVS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0 || address16) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOVS;
        set_reg(out, 1, HB_REG_RSI, opcode == 0xA4 ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xA6 || opcode == 0xA7) {
        /* CMPS m8/m16/m32. REPNE/REPE are carried as an immediate mode. */
        if (prefix_f0 || address16) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_CMPS;
        set_reg(out, 1, HB_REG_RSI, opcode == 0xA6 ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAC || opcode == 0xAD) {
        /* LODS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0 || address16) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LODS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAC ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xAE || opcode == 0xAF) {
        /* SCAS m8/m16/m32. REPNE/REPE are carried as an immediate mode. */
        if (prefix_f0 || address16) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_SCAS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAE ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAA || opcode == 0xAB) {
        /* STOS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0 || address16) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_STOS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAA ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }

    if (opcode == 0x0F) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (op2 == 0x1F) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_NOP;
            return parse_modrm_ext(d, modrm, 1, out, 1);
        }
        if (op2 == 0x0D || op2 == 0x18) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_NOP;
            return parse_modrm_ext(d, modrm, 1, out, 1);
        }
        d->pos--;
    }

    if (prefix_f0) {
        if (opcode <= 0x3B && ((opcode & 7) <= 1)) {
            int ins;
            switch (opcode & 0x38) {
                case 0x00: ins = HB_INS_ADD; break;
                case 0x08: ins = HB_INS_OR;  break;
                case 0x10: ins = HB_INS_ADC; break;
                case 0x18: ins = HB_INS_SBB; break;
                case 0x20: ins = HB_INS_AND; break;
                case 0x28: ins = HB_INS_SUB; break;
                case 0x30: ins = HB_INS_XOR; break;
                default: return HB_ERR_UNSUPPORTED_OPCODE;
            }
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = ins;
            out->writes_flags = true;
            return parse_modrm(d, modrm, (opcode & 1) ? (operand16 ? 2 : 4) : 1,
                               out, 1, 2, true);
        }
        if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if ((modrm >> 6) == 3 || ext == 7) return HB_ERR_UNSUPPORTED_OPCODE;
            if (ext == 0) out->opcode = HB_INS_ADD;
            else if (ext == 1) out->opcode = HB_INS_OR;
            else if (ext == 2) out->opcode = HB_INS_ADC;
            else if (ext == 3) out->opcode = HB_INS_SBB;
            else if (ext == 4) out->opcode = HB_INS_AND;
            else if (ext == 5) out->opcode = HB_INS_SUB;
            else out->opcode = HB_INS_XOR;
            out->writes_flags = true;
            uint8_t sz = (opcode == 0x80) ? 1 : (operand16 ? 2 : 4);
            hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
            if (r != HB_OK) return r;
            if (opcode == 0x80) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, read_s8(d), 1);
            } else if (opcode == 0x81) {
                if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
            } else {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s8(d), sz);
            }
            return HB_OK;
        }
        if (opcode == 0xF6 || opcode == 0xF7) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            if (ext == 2) out->opcode = HB_INS_NOT;
            else if (ext == 3) out->opcode = HB_INS_NEG;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            out->writes_flags = (ext == 3);
            return parse_modrm_ext(d, modrm, opcode == 0xF6 ? 1 : (operand16 ? 2 : 4), out, 1);
        }
        if (opcode == 0xFE) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            if (ext == 0) out->opcode = HB_INS_INC;
            else if (ext == 1) out->opcode = HB_INS_DEC;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            out->writes_flags = true;
            return parse_modrm_ext(d, modrm, 1, out, 1);
        }
        if (opcode == 0x0F) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t op2 = read_u8(d);
            if (op2 == 0xB0 || op2 == 0xB1 || op2 == 0xC0 || op2 == 0xC1) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                out->opcode = (op2 == 0xB0 || op2 == 0xB1) ? HB_INS_CMPXCHG : HB_INS_XADD;
                out->writes_flags = true;
                return parse_modrm(d, modrm, op2 == 0xB0 || op2 == 0xC0 ? 1 : 4, out, 1, 2, true);
            }
            if (op2 == 0xC7) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if (((modrm >> 3) & 7) != 1 || (modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                out->opcode = HB_INS_CMPXCHG8B;
                out->writes_flags = true;
                return parse_modrm_ext(d, modrm, 8, out, 1);
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (opcode == 0xFF) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            if (ext == 0) {
                out->opcode = HB_INS_INC;
                out->writes_flags = true;
                return parse_modrm_ext(d, modrm, operand16 ? 2 : 4, out, 1);
            }
            if (ext == 1) {
                out->opcode = HB_INS_DEC;
                out->writes_flags = true;
                return parse_modrm_ext(d, modrm, operand16 ? 2 : 4, out, 1);
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode == 0x0F && (prefix_f2 || prefix_f3)) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (prefix_f3 && (op2 == 0xBC || op2 == 0xBD)) {
            /* TZCNT/LZCNT r16/32, r/m16/32. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xBC) ? HB_INS_TZCNT : HB_INS_LZCNT;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0x70) {
            /* PSHUFLW/PSHUFHW xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = prefix_f2 ? HB_INS_PSHUFLW : HB_INS_PSHUFHW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x7E) {
            /* MOVQ xmm, xmm/m64. Ported from the x64 SSE transfer family. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0xE6) {
            /* CVTDQ2PD xmm, xmm/m64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTDQ2PD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x5B) {
            /* CVTTPS2DQ xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTTPS2DQ;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x5A) {
            /* F3/F2 scalar converts: CVTSS2SD and CVTSD2SS. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mem_size = prefix_f3 ? 4 : 8;
            out->opcode = prefix_f3 ? HB_INS_CVTSS2SD : HB_INS_CVTSD2SS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (op2 == 0x58 || op2 == 0x5C) {
            /* Scalar ADD/SUB: ADDSS/SUBSS and ADDSD/SUBSD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mem_size = prefix_f3 ? 4 : 8;
            if (prefix_f3) out->opcode = (op2 == 0x58) ? HB_INS_ADDSS : HB_INS_SUBSS;
            else out->opcode = (op2 == 0x58) ? HB_INS_ADDSD : HB_INS_SUBSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x5E) {
            /* DIVSD xmm, xmm/m64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_DIVSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (prefix_f2 && op2 == 0x59) {
            /* MULSD xmm, xmm/m64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MULSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 8;
            return HB_OK;
        }
        if (prefix_f3 && (op2 == 0x5E || op2 == 0x59)) {
            /* DIVSS/MULSS xmm, xmm/m32. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0x5E) ? HB_INS_DIVSS : HB_INS_MULSS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 4;
            return HB_OK;
        }
        if (op2 == 0x5D || op2 == 0x5F) {
            /* Scalar MIN/MAX: MINSS/MAXSS and MINSD/MAXSD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mem_size = prefix_f3 ? 4 : 8;
            if (prefix_f3) out->opcode = (op2 == 0x5D) ? HB_INS_MINSS : HB_INS_MAXSS;
            else out->opcode = (op2 == 0x5D) ? HB_INS_MINSD : HB_INS_MAXSD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
            op2 == 0x6F || op2 == 0x7F) {
            /* MOVUPS/MOVAPS/MOVDQA/MOVDQU and scalar MOVSS/MOVSD variants. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            bool scalar_move = (op2 == 0x10 || op2 == 0x11);
            uint8_t move_size = scalar_move ? (prefix_f3 ? 4 : 8) : 16;
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, move_size, out, 1, 2,
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
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode == 0x0F) {
        size_t saved_pos = d->pos;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (operand16 && op2 == 0x38) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            if (op3 == 0x00) {
                /* SSSE3 PSHUFB xmm, xmm/m128. */
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = HB_INS_PSHUFB;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                return HB_OK;
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (operand16 && op2 == 0x70) {
            /* PSHUFD xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_PSHUFD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            return HB_OK;
        }
        if (operand16 && op2 == 0xC4) {
            /* PINSRW xmm, r/m16, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_PINSRW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 2, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            return HB_OK;
        }
        if (operand16 && op2 == 0xC5) {
            /* PEXTRW r32, xmm, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_PEXTRW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (!out->op2.is_reg) return HB_ERR_UNSUPPORTED_OPCODE;
            mark_xmm_operand(out, 2);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
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
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = 4;
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && op2 == 0x6E) {
            /* MOVD xmm, r/m32. Ported from the x64 SSE transfer family. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            out->op2.size = 4;
            return HB_OK;
        }
        if (operand16 && op2 == 0x7E) {
            /* MOVD r/m32, xmm. Ported from the x64 SSE transfer family. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            out->op1.size = 4;
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && op2 == 0xD6) {
            /* MOVQ xmm/m64, xmm. Ported from the x64 SSE transfer family. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOVD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, true);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op1.is_mem) out->op1.size = 8;
            return HB_OK;
        }
        if (op2 == 0x12 || op2 == 0x13) {
            /* MOVLPS/MOVLPD memory forms: low qword load/store.  The ModRM
             * register forms alias high-lane moves and need lane-specific IR. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, op2 == 0x13);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, op2 == 0x13 ? 2 : 1);
            if (op2 == 0x13) out->op2.size = 8;
            else out->op1.size = 8;
            return HB_OK;
        }
        if ((!operand16 && op2 == 0x5B) || (operand16 && op2 == 0x5B)) {
            /* CVTDQ2PS/CVTPS2DQ xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_CVTPS2DQ : HB_INS_CVTDQ2PS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x5A) {
            /* CVTPS2PD/CVTPD2PS xmm, xmm/m64/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mem_size = operand16 ? 16 : 8;
            out->opcode = operand16 ? HB_INS_CVTPD2PS : HB_INS_CVTPS2PD;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if (op2 == 0x58 || op2 == 0x5C) {
            /* Packed ADD/SUB: ADDPS/SUBPS and ADDPD/SUBPD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (operand16) out->opcode = (op2 == 0x58) ? HB_INS_ADDPD : HB_INS_SUBPD;
            else out->opcode = (op2 == 0x58) ? HB_INS_ADDPS : HB_INS_SUBPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x5D || op2 == 0x5F) {
            /* Packed MIN/MAX: MINPS/MAXPS and MINPD/MAXPD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (operand16) out->opcode = (op2 == 0x5D) ? HB_INS_MINPD : HB_INS_MAXPD;
            else out->opcode = (op2 == 0x5D) ? HB_INS_MINPS : HB_INS_MAXPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if ((op2 == 0x2E || op2 == 0x2F) && !prefix_f2 && !prefix_f3) {
            /* COMISS/UCOMISS and COMISD/UCOMISD share ordered flag semantics here. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_COMISD : HB_INS_COMISS;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 8 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = operand16 ? 8 : 4;
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
            op2 == 0x6F || op2 == 0x7F) {
            /* Packed 128-bit XMM moves: MOVUPS/MOVAPS/MOVDQA/MOVDQU. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2,
                                        (op2 == 0x11 || op2 == 0x29 || op2 == 0x7F));
            if (r != HB_OK) return r;
            mark_xmm_operands(out);
            return HB_OK;
        }
        if ((!operand16 && op2 >= 0x54 && op2 <= 0x57) ||
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
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        d->pos = saved_pos;
    }

    if ((operand16 && !((opcode <= 0x3D) && ((opcode & 7) <= 5)) &&
          !(opcode >= 0x88 && opcode <= 0x8B) && opcode != 0x8D &&
          opcode != 0x0F &&
          !(opcode >= 0xA0 && opcode <= 0xA3) && !(opcode >= 0xB0 && opcode <= 0xBF) &&
          opcode != 0x9C && opcode != 0x9D &&
          opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
          opcode != 0x85 && opcode != 0xA9 &&
          opcode != 0x69 && opcode != 0x6B &&
          opcode != 0xC0 && opcode != 0xC1 &&
          !(opcode >= 0xD0 && opcode <= 0xD3) &&
          opcode != 0xC6 && opcode != 0xC7 && opcode != 0xC9) ||
        address16 || prefix_f2 || prefix_f3) {
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode >= 0xd8 && opcode <= 0xdf) {
        return decode_x87(d, opcode, out);
    }

    if (opcode == 0x04 || opcode == 0x05 || opcode == 0x0C || opcode == 0x0D ||
        opcode == 0x14 || opcode == 0x15 || opcode == 0x1C || opcode == 0x1D ||
        opcode == 0x24 || opcode == 0x25 || opcode == 0x2C || opcode == 0x2D ||
        opcode == 0x34 || opcode == 0x35 || opcode == 0x3C || opcode == 0x3D) {
        switch (opcode & 0x3c) {
            case 0x04: out->opcode = HB_INS_ADD; break;
            case 0x0c: out->opcode = HB_INS_OR;  break;
            case 0x14: out->opcode = HB_INS_ADC; break;
            case 0x1c: out->opcode = HB_INS_SBB; break;
            case 0x24: out->opcode = HB_INS_AND; break;
            case 0x2c: out->opcode = HB_INS_SUB; break;
            case 0x34: out->opcode = HB_INS_XOR; break;
            case 0x3c: out->opcode = HB_INS_CMP; break;
        }
        uint8_t size = (opcode & 1) ? (operand16 ? 2 : 4) : 1;
        out->writes_flags = true;
        if (!can_read(d, size)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, size);
        set_imm(out, 2, size == 1 ? read_s8(d) : size == 2 ? read_s16(d) : (int64_t)read_s32(d), size);
        return HB_OK;
    }

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
        return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, true);
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
        return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
    }
    if (opcode == 0x8D) {
        /* LEA r16/32, m */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_LEA;
        out->writes_flags = false;
        return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
    }
    if (opcode >= 0xB0 && opcode <= 0xB7) {
        /* MOV r8, imm8 */
        uint8_t reg_offset = 0;
        int reg = reg8_idx(opcode & 7, &reg_offset);
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t imm = read_s8(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        set_reg_ex(out, 1, reg, 1, reg_offset);
        set_imm(out, 2, imm, 1);
        return HB_OK;
    }
    if (opcode >= 0xB8 && opcode <= 0xBF) {
        /* MOV r16/32, imm16/32 */
        int reg = opcode & 7;
        uint8_t sz = operand16 ? 2 : 4;
        if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        set_reg(out, 1, reg, sz);
        set_imm(out, 2, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
        return HB_OK;
    }
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        /* MOV AL/EAX <-> moffs. In x86 this is an absolute 32-bit offset;
           segment prefixes still apply, e.g. FS:A1 for TEB loads. */
        uint8_t sz = (opcode == 0xA0 || opcode == 0xA2) ? 1 : (operand16 ? 2 : 4);
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        uint32_t moffs = (uint32_t)read_s32(d);
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        if (opcode == 0xA0 || opcode == 0xA1) {
            set_reg(out, 1, HB_REG_RAX, sz);
            set_mem(out, 2, -1, -1, 1, moffs, sz);
        } else {
            set_mem(out, 1, -1, -1, 1, moffs, sz);
            set_reg(out, 2, HB_REG_RAX, sz);
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
        hb_result_t r = parse_modrm_ext(d, modrm, 1, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_s8(d), 1);
        return HB_OK;
    }
    if (opcode == 0xC7) {
        /* MOV r/m16/32, imm16/32 */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        uint8_t sz = operand16 ? 2 : 4;
        hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
        return HB_OK;
    }

    /* Group: 0x80/0x81/0x83 immediate group. Operand-size prefix selects
       r/m16 for 0x81/0x83; 0x80 remains byte-sized. */
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        uint8_t sz = (opcode == 0x80) ? 1 : (operand16 ? 2 : 4);

        if (ext == 0) out->opcode = HB_INS_ADD;
        else if (ext == 1) out->opcode = HB_INS_OR;
        else if (ext == 2) out->opcode = HB_INS_ADC;
        else if (ext == 3) out->opcode = HB_INS_SBB;
        else if (ext == 4) out->opcode = HB_INS_AND;
        else if (ext == 5) out->opcode = HB_INS_SUB;
        else if (ext == 6) out->opcode = HB_INS_XOR;
        else if (ext == 7) out->opcode = HB_INS_CMP;
        out->writes_flags = true;

        hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
        if (r != HB_OK) return r;

        if (opcode == 0x80) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_s8(d), 1);
        } else if (opcode == 0x81) {
            if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
        } else { /* 0x83 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s8(d), sz);
        }
        return HB_OK;
    }

    if (opcode == 0x69 || opcode == 0x6B) {
        /* IMUL r16/32, r/m16/32, imm16/32/8. Mirrors the x64 69/6B path. */
        uint8_t sz = operand16 ? 2 : 4;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_IMUL;
        out->writes_flags = true;
        hb_result_t r = parse_modrm(d, modrm, sz, out, 1, 2, false);
        if (r != HB_OK) return r;
        if (opcode == 0x69) {
            if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
        } else {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, (int64_t)read_s8(d), sz);
        }
        return HB_OK;
    }

    if (opcode <= 0x3B && ((opcode & 7) <= 3)) {
        int ins;
        uint8_t modrm, size;
        switch (opcode & 0x38) {
            case 0x00: ins = HB_INS_ADD; break;
            case 0x08: ins = HB_INS_OR;  break;
            case 0x10: ins = HB_INS_ADC; break;
            case 0x18: ins = HB_INS_SBB; break;
            case 0x20: ins = HB_INS_AND; break;
            case 0x28: ins = HB_INS_SUB; break;
            case 0x30: ins = HB_INS_XOR; break;
            case 0x38: ins = HB_INS_CMP; break;
            default: return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        modrm = read_u8(d);
        size = (opcode & 1) ? (operand16 ? 2 : 4) : 1;
        out->opcode = ins;
        out->writes_flags = true;
        return parse_modrm(d, modrm, size, out, 1, 2, (opcode & 2) == 0);
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
    if (opcode == 0x8F) {
        /* POP r/m32. Used by i386 ntdll SEH restore paths such as
           "pop dword ptr fs:[0]". */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_POP;
        out->stack_delta = 4;
        return parse_modrm_ext(d, modrm, 4, out, 1);
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
    if (opcode == 0xC9) {
        if (address16 || prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LEAVE;
        out->writes_flags = false;
        out->stack_delta = operand16 ? 2 : 4;
        set_reg(out, 1, HB_REG_RBP, operand16 ? 2 : 4);
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
        if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
            /* SHLD/SHRD r/m16/32, r16/32, imm8/CL. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xA4 || op2 == 0xA5) ? HB_INS_SHLD : HB_INS_SHRD;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            if (op2 == 0xA4 || op2 == 0xAC) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
            } else {
                set_reg(out, 3, HB_REG_X86_ECX, 1);
            }
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
        if (op2 == 0xB0 || op2 == 0xB1 || op2 == 0xC0 || op2 == 0xC1) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xB0 || op2 == 0xB1) ? HB_INS_CMPXCHG : HB_INS_XADD;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, op2 == 0xB0 || op2 == 0xC0 ? 1 : 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xC7) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (((modrm >> 3) & 7) != 1 || (modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = HB_INS_CMPXCHG8B;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, 8, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xBA) {
            /* Group 8: BT/BTS/BTR/BTC r/m16/32, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if (ext == 4) out->opcode = HB_INS_BT;
            else if (ext == 5) out->opcode = HB_INS_BTS;
            else if (ext == 6) out->opcode = HB_INS_BTR;
            else if (ext == 7) out->opcode = HB_INS_BTC;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, operand16 ? 2 : 4, out, 1);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_u8(d), 1);
            return HB_OK;
        }
        if (op2 == 0xAF) {
            /* IMUL r16/32, r/m16/32. Mirrors the x64 0F AF path. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_IMUL;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            /* BT/BTS/BTR/BTC r/m16/32, r16/32. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xA3) out->opcode = HB_INS_BT;
            else if (op2 == 0xAB) out->opcode = HB_INS_BTS;
            else if (op2 == 0xB3) out->opcode = HB_INS_BTR;
            else out->opcode = HB_INS_BTC;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, true);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xBC || op2 == 0xBD) {
            /* BSF/BSR r16/32, r/m16/32. Match the x64 decoder and normalize
               BSF through the existing TZCNT IR for non-zero sources. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xBC) ? HB_INS_TZCNT : HB_INS_BSR;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            /* BSWAP r32. */
            out->opcode = HB_INS_BSWAP;
            out->writes_flags = false;
            set_reg(out, 1, op2 - 0xC8, 4);
            return HB_OK;
        }
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t src_size = (op2 == 0xB6 || op2 == 0xBE) ? 1 : 2;
            out->opcode = (op2 == 0xB6 || op2 == 0xB7) ? HB_INS_MOVZX : HB_INS_MOVSX;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (src_size == 1 && out->op2.is_reg) {
                uint8_t src_offset = 0;
                out->op2.reg = reg8_idx(out->rm, &src_offset);
                out->op2.reg_offset = src_offset;
            }
            out->op2.size = src_size;
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
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
        uint8_t sz = (opcode == 0xC0) ? 1 : (operand16 ? 2 : 4);
        hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
        if (r != HB_OK) return r;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        set_imm(out, 2, read_u8(d) & 0x1F, 1);
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
        uint8_t sz = (opcode == 0xD0) ? 1 : (operand16 ? 2 : 4);
        hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
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
        uint8_t sz = (opcode == 0xD2) ? 1 : (operand16 ? 2 : 4);
        hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
        if (r != HB_OK) return r;
        set_reg(out, 2, HB_REG_X86_ECX, 1); /* CL */
        return HB_OK;
    }

    /* Group: 0xFE byte INC/DEC. Mirrors the x64 FE /0,/1 path. */
    if (opcode == 0xFE) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        if (ext == 0) out->opcode = HB_INS_INC;
        else if (ext == 1) out->opcode = HB_INS_DEC;
        else return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        return parse_modrm_ext(d, modrm, 1, out, 1);
    }

    /* Group: TEST / NOT / NEG / MUL / IMUL / DIV / IDIV (F6/F7). */
    if (opcode == 0xF6 || opcode == 0xF7) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t ext = (modrm >> 3) & 7;
        uint8_t sz = (opcode == 0xF6) ? 1 : 4;
        if (ext == 0) {
            out->opcode = HB_INS_TEST;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, sz, out, 1);
            if (r != HB_OK) return r;
            if (opcode == 0xF6) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, read_s8(d), 1);
            } else {
                if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 2, (int64_t)read_s32(d), 4);
            }
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
        return parse_modrm_ext(d, modrm, sz, out, 1);
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
        return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, true);
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
        uint8_t sz = operand16 ? 2 : 4;
        if (!can_read(d, sz)) return HB_ERR_DECODE_FAILED;
        set_reg(out, 1, 0, sz);
        set_imm(out, 2, sz == 2 ? (int64_t)read_s16(d) : (int64_t)read_s32(d), sz);
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
