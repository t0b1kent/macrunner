#include "hb_decoder.h"
#include "hb_ir.h"
#include <string.h>
#include <stdlib.h>

#define HB_X86_DEFAULT_MXCSR 0x1f80u

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

/* 0F38 map: SSSE3 family (subset that has a non-VEC HB_INS_*). */
static int ssse3_0f38_opcode(uint8_t op) {
    switch (op) {
        case 0x00: return HB_INS_PSHUFB;
        case 0x01: return HB_INS_PHADDW;
        case 0x02: return HB_INS_PHADDD;
        case 0x03: return HB_INS_PHADDSW;
        case 0x04: return HB_INS_PMADDUBSW;
        case 0x05: return HB_INS_PHSUBW;
        case 0x06: return HB_INS_PHSUBD;
        case 0x07: return HB_INS_PHSUBSW;
        case 0x08: return HB_INS_PSIGNB;
        case 0x09: return HB_INS_PSIGNW;
        case 0x0a: return HB_INS_PSIGND;
        case 0x0b: return HB_INS_PMULHRSW;
        case 0x1c: return HB_INS_PABSB;
        case 0x1d: return HB_INS_PABSW;
        case 0x1e: return HB_INS_PABSD;
        default: return 0;
    }
}

/* 0F38 map: SSE4.1 / SSE4.2 / AES / GF2P8MULB user-mode subset. */
static int sse41_0f38_opcode(uint8_t op) {
    switch (op) {
        case 0x10: return HB_INS_PBLENDVB;
        case 0x14: return HB_INS_BLENDVPS;
        case 0x15: return HB_INS_BLENDVPD;
        case 0x17: return HB_INS_PTEST;
        case 0x20: return HB_INS_PMOVSXBW;
        case 0x21: return HB_INS_PMOVSXBD;
        case 0x22: return HB_INS_PMOVSXBQ;
        case 0x23: return HB_INS_PMOVSXWD;
        case 0x24: return HB_INS_PMOVSXWQ;
        case 0x25: return HB_INS_PMOVSXDQ;
        case 0x28: return HB_INS_PMULDQ;
        case 0x29: return HB_INS_PCMPEQQ;
        case 0x2b: return HB_INS_PACKUSDW;
        case 0x30: return HB_INS_PMOVZXBW;
        case 0x31: return HB_INS_PMOVZXBD;
        case 0x32: return HB_INS_PMOVZXBQ;
        case 0x33: return HB_INS_PMOVZXWD;
        case 0x34: return HB_INS_PMOVZXWQ;
        case 0x35: return HB_INS_PMOVZXDQ;
        case 0x37: return HB_INS_PCMPGTQ;
        case 0x38: return HB_INS_PMINSB;
        case 0x39: return HB_INS_PMINSD;
        case 0x3a: return HB_INS_PMINUW;
        case 0x3b: return HB_INS_PMINUD;
        case 0x3c: return HB_INS_PMAXSB;
        case 0x3d: return HB_INS_PMAXSD;
        case 0x3e: return HB_INS_PMAXUW;
        case 0x3f: return HB_INS_PMAXUD;
        case 0x40: return HB_INS_PMULLD;
        case 0x41: return HB_INS_PHMINPOSUW;
        case 0xcf: return HB_INS_GF2P8MULB;
        case 0xf0: return HB_INS_CRC32;
        case 0xf1: return HB_INS_CRC32;
        case 0xf6: return HB_INS_ADCX;
        case 0xf7: return HB_INS_ADOX;
        case 0xdb: return HB_INS_AESIMC;
        case 0xdc: return HB_INS_AESENC;
        case 0xdd: return HB_INS_AESENCLAST;
        case 0xde: return HB_INS_AESDEC;
        case 0xdf: return HB_INS_AESDECLAST;
        /* SHA-NI */
        case 0xc8: return HB_INS_SHA1NEXTE;
        case 0xc9: return HB_INS_SHA1MSG1;
        case 0xca: return HB_INS_SHA1MSG2;
        case 0xcb: return HB_INS_SHA256RNDS2;
        case 0xcc: return HB_INS_SHA256MSG1;
        case 0xcd: return HB_INS_SHA256MSG2;
        default: return 0;
    }
}

/* 0F3A map: SSE4.1 imm8 forms + AES + PCLMULQDQ + SHA1RNDS4. */
static int sse41_0f3a_opcode(uint8_t op) {
    switch (op) {
        case 0x0c: return HB_INS_BLENDPS;
        case 0x0d: return HB_INS_BLENDPD;
        case 0x0e: return HB_INS_PBLENDW;
        case 0x0f: return HB_INS_PALIGNR;
        case 0x44: return HB_INS_PCLMULQDQ;
        case 0x60: return HB_INS_PCMPESTRM;
        case 0x61: return HB_INS_PCMPESTRI;
        case 0x62: return HB_INS_PCMPISTRM;
        case 0x63: return HB_INS_PCMPISTRI;
        case 0xcc: return HB_INS_SHA1RNDS4;
        case 0xdf: return HB_INS_AESKEYGENASSIST;
        default: return 0;
    }
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
            if (modrm == 0xe8) {
                /* FLD1: push 1.0 */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -1, 1);
                return HB_OK;
            }
            if (modrm == 0xee) {
                /* FLDZ: push 0.0 */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -2, 1);
                return HB_OK;
            }
            if (modrm == 0xe9) {
                /* FLDL2T: push log2(10) */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -3, 1);
                return HB_OK;
            }
            if (modrm == 0xea) {
                /* FLDL2E: push log2(e) */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -4, 1);
                return HB_OK;
            }
            if (modrm == 0xeb) {
                /* FLDPI: push pi */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -5, 1);
                return HB_OK;
            }
            if (modrm == 0xec) {
                /* FLDLG2: push log10(2) */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -6, 1);
                return HB_OK;
            }
            if (modrm == 0xed) {
                /* FLDLN2: push ln(2) */
                out->opcode = HB_INS_X87_FLD;
                set_imm(out, 1, -7, 1);
                return HB_OK;
            }
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FLD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FXCH;
            else if (modrm == 0xfc) {
                /* FRNDINT — must come before the 0xf8..0xff FUCOMPI range. */
                out->opcode = HB_INS_X87_FRNDINT;
                return HB_OK;
            }
            else if (modrm == 0xd0) {
                /* FNOP — FPU no-op. */
                out->opcode = HB_INS_X87_FNOP;
                return HB_OK;
            }
            else if (modrm >= 0xd1 && modrm <= 0xd7) out->opcode = HB_INS_X87_FSTP; /* FSTPNCE — undocumented */
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FSTP;
            else if (modrm == 0xe0) { out->opcode = HB_INS_X87_FCHS; return HB_OK; }
            else if (modrm == 0xe1) { out->opcode = HB_INS_X87_FABS; return HB_OK; }
            else if (modrm == 0xe4) { out->opcode = HB_INS_X87_FTST; return HB_OK; }
            else if (modrm == 0xe5) { out->opcode = HB_INS_X87_FXAM; return HB_OK; }
            else if (modrm == 0xf6) { out->opcode = HB_INS_X87_FDECSTP; return HB_OK; }
            else if (modrm == 0xf7) { out->opcode = HB_INS_X87_FINCSTP; return HB_OK; }
            else if (modrm >= 0xe8 && modrm <= 0xee) out->opcode = HB_INS_X87_FLD;  /* FLD1/FLDL2T/.../FLDZ */
            else if (modrm == 0xf0) { out->opcode = HB_INS_X87_F2XM1;   return HB_OK; }
            else if (modrm == 0xf1) { out->opcode = HB_INS_X87_FYL2X;   return HB_OK; }
            else if (modrm == 0xf2) { out->opcode = HB_INS_X87_FPTAN;   return HB_OK; }
            else if (modrm == 0xf3) { out->opcode = HB_INS_X87_FPATAN;  return HB_OK; }
            else if (modrm == 0xf4) { out->opcode = HB_INS_X87_FXTRACT; return HB_OK; }
            else if (modrm == 0xf5) { out->opcode = HB_INS_X87_FPREM1;  return HB_OK; }
            else if (modrm == 0xf8) { out->opcode = HB_INS_X87_FPREM;   return HB_OK; }
            else if (modrm == 0xf9) { out->opcode = HB_INS_X87_FYL2XP1; return HB_OK; }  /* per capstone, D9 F9 = FYL2XP1 */
            else if (modrm == 0xfa) { out->opcode = HB_INS_X87_FSQRT;   return HB_OK; }
            else if (modrm == 0xfb) { out->opcode = HB_INS_X87_FSINCOS; return HB_OK; }
            else if (modrm == 0xfc) { out->opcode = HB_INS_X87_FRNDINT; return HB_OK; }
            else if (modrm == 0xfd) { out->opcode = HB_INS_X87_FSCALE;  return HB_OK; }
            else if (modrm == 0xfe) { out->opcode = HB_INS_X87_FSIN;    return HB_OK; }
            else if (modrm == 0xff) { out->opcode = HB_INS_X87_FCOS;    return HB_OK; }
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
            else if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FCMOV; /* FCMOVNB */
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FCMOV; /* FCMOVNE */
            else if (modrm >= 0xd0 && modrm <= 0xd7) out->opcode = HB_INS_X87_FCMOV; /* FCMOVNBE */
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCMOV; /* FCMOVNU */
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FUCOMI;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FCOMI;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xda) {
            /* FCMOVB/FCMOVE/FCMOVBE/FCMOVU + FUCOMPP (Pentium Pro+). */
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FCMOV; /* FCMOVB */
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FCMOV; /* FCMOVE */
            else if (modrm >= 0xd0 && modrm <= 0xd7) out->opcode = HB_INS_X87_FCMOV; /* FCMOVBE */
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCMOV; /* FCMOVU */
            else if (modrm == 0xe9) {
                /* FUCOMPP ST(0), ST(1) — pop twice. */
                out->opcode = HB_INS_X87_FCOMPP;
                set_imm(out, 1, 1, 1);
                return HB_OK;
            }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xdc) {
            /* FADD/FSUB/FMUL/FDIV ST(i), ST (mod=3 form). */
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FADD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FMUL;
            else if (modrm >= 0xe0 && modrm <= 0xe7) out->opcode = HB_INS_X87_FSUB;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FSUBR;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FDIV;
            else if (modrm >= 0xf8) out->opcode = HB_INS_X87_FDIVR;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xdd) {
            /* FFREE / FST / FSTP / FUCOM / FUCOMP (mod=3 form). */
            if (modrm >= 0xc0 && modrm <= 0xc7) {
                out->opcode = HB_INS_X87_FFREE;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xd0 && modrm <= 0xd7) {
                out->opcode = HB_INS_X87_FST;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xd8 && modrm <= 0xdf) {
                out->opcode = HB_INS_X87_FSTP;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xe0 && modrm <= 0xe7) {
                out->opcode = HB_INS_X87_FUCOM;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xe8 && modrm <= 0xef) {
                out->opcode = HB_INS_X87_FUCOMP;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (opcode == 0xdf) {
            /* FFREEP ST(i) and FUCOMPI/FCOMPI (mod=3 form). */
            if (modrm >= 0xc0 && modrm <= 0xc7) {
                out->opcode = HB_INS_X87_FFREEP;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xe8 && modrm <= 0xef) {
                out->opcode = HB_INS_X87_FUCOMPI;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            else if (modrm >= 0xf0 && modrm <= 0xf7) {
                out->opcode = HB_INS_X87_FCOMPI;
                set_imm(out, 1, sti, 1);
                return HB_OK;
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
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
            else if (reg_op == 4) { out->opcode = HB_INS_X87_FLDENV; size = 28; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FLDCW; size = 2; }
            else if (reg_op == 6) { out->opcode = HB_INS_X87_FNSTENV; size = 28; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FNSTCW; size = 2; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xda:
            /* FIADD/FIMUL/FICOM/FICOMP/FISUB/FISUBR/FIDIV/FIDIVR m32int. */
            switch (reg_op) {
                case 0: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 1: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 2: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 3: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 4: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 5: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 6: out->opcode = HB_INS_X87_FI; size = 4; break;
                case 7: out->opcode = HB_INS_X87_FI; size = 4; break;
            }
            break;
        case 0xdb:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 4; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FIST; size = 4; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FISTP; size = 4; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FLD; size = 10; }  /* FLD m80 (extended) */
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FSTP; size = 10; } /* FSTP m80 (extended) */
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xde:
            /* FIADD/FIMUL/FICOM/FICOMP/FISUB/FISUBR/FIDIV/FIDIVR m16int. */
            switch (reg_op) {
                case 0: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 1: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 2: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 3: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 4: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 5: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 6: out->opcode = HB_INS_X87_FI; size = 2; break;
                case 7: out->opcode = HB_INS_X87_FI; size = 2; break;
            }
            break;
        case 0xdd:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FLD; size = 8; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 8; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FSTP; size = 8; }
            else if (reg_op == 4) { out->opcode = HB_INS_X87_FRSTOR; size = 108; }
            else if (reg_op == 6) { out->opcode = HB_INS_X87_FNSAVE; size = 108; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FNSTSW; size = 2; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xdf:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 2; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FIST; size = 2; }
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

    /* F2/F3 (REPNE/REPZ) are ignored on most non-string ops per Intel SDM
     * Vol 2 (and used as XACQUIRE/XRELEASE for HLE; we don't model HLE).
     * We KEEP prefix_f2/prefix_f3 for the 0F escape map and for string
     * ops, since the 0F SSE/SSE2/SSE3 family uses F2/F3 as the operand-type
     * selector (MOVSD vs MOVSS, HADDPS vs HSUBPS, ...). */
    if (can_read(d, 1) && d->code[d->pos] != 0x0F) {
        uint8_t o = d->code[d->pos];
        bool is_string_op = (o >= 0x6C && o <= 0x6F) ||  /* INS/OUTS */
                            (o >= 0xA4 && o <= 0xA7) ||  /* MOVS/CMPS */
                            (o >= 0xAA && o <= 0xAF) ||  /* STOS/LODS/SCAS */
                            o == 0x90;                   /* NOP/PAUSE */
        if (!is_string_op) {
            prefix_f2 = false;
            prefix_f3 = false;
        }
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
    if (opcode == 0x60 || opcode == 0x61) {
        /* PUSHA / PUSHAD (0x60) and POPA / POPAD (0x61).
         * PUSHAD is the 32-bit form (default in 32-bit mode); PUSHA is the 16-bit
         * form (selected with the 0x66 operand-size prefix). Likewise for POPA. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = (opcode == 0x60) ? HB_INS_PUSHA : HB_INS_POPA;
        out->writes_flags = false;
        if (opcode == 0x60) {
            /* Lift needs the element size to choose 16- or 32-bit pushes. */
            set_imm(out, 1, operand16 ? 2 : 4, 1);
        } else {
            out->op1.size = operand16 ? 2 : 4;
        }
        return HB_OK;
    }
    if (opcode == 0x06 || opcode == 0x0E || opcode == 0x16 || opcode == 0x1E) {
        /* PUSH ES/CS/SS/DS — segment register push (32-bit user mode only).
         * 64-bit mode #UDs these; we reject by checking that we're not in 64-bit
         * (the caller should not invoke hb_decode_x86 in that mode anyway, but
         * we still validate the prefix set: LOCK, REP/REPNE, and address-size
         * override are illegal). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_PUSH_SEG;
        out->writes_flags = false;
        out->stack_delta = operand16 ? 2 : 4;
        /* Encode the segment selector in op2.imm — the lift+interpreter use it
         * to identify which segment (0=ES, 1=CS, 2=SS, 3=DS, 4=FS, 5=GS). */
        set_imm(out, 1, 0, out->stack_delta);
        set_imm(out, 2, (opcode >> 3) & 7, 1);
        return HB_OK;
    }
    if (opcode == 0x07 || opcode == 0x17 || opcode == 0x1F) {
        /* POP ES/SS/DS — segment register pop. POP CS is undefined in 32-bit
         * user mode and #UDs; POP FS/GS (0x0F A1 / 0x0F A9) is the 0F-escape
         * form, handled in the 0F map. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_POP_SEG;
        out->writes_flags = false;
        out->stack_delta = -(int)(operand16 ? 2 : 4);
        set_imm(out, 1, 0, operand16 ? 2 : 4);
        set_imm(out, 2, (opcode >> 3) & 7, 1);
        return HB_OK;
    }
    if (opcode == 0xF8 || opcode == 0xF9 || opcode == 0xF5 ||
        opcode == 0xFA || opcode == 0xFB ||
        opcode == 0xFC || opcode == 0xFD) {
        /* Flag ops: CLC (F8), STC (F9), CMC (F5), CLI (FA), STI (FB),
         * CLD (FC), STD (FD). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->writes_flags = true;
        switch (opcode) {
            case 0xF8: out->opcode = HB_INS_CLC; return HB_OK;
            case 0xF9: out->opcode = HB_INS_STC; return HB_OK;
            case 0xF5: out->opcode = HB_INS_CMC; return HB_OK;
            case 0xFA: out->opcode = HB_INS_CLI; return HB_OK;
            case 0xFB: out->opcode = HB_INS_STI; return HB_OK;
            case 0xFC: out->opcode = HB_INS_CLD; return HB_OK;
            case 0xFD: out->opcode = HB_INS_STD; return HB_OK;
            default: return HB_ERR_UNSUPPORTED_OPCODE;
        }
    }
    if (opcode == 0x98) {
        /* CBW / CWDE — sign-extend AL into AX (16-bit opsize) or AX into EAX
         * (32-bit opsize). Valid in 32-bit user mode; no prefix allowed. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_CWDE;
        out->reads_flags = false;
        out->writes_flags = false;
        set_imm(out, 1, operand16 ? 1 : 2, 1);
        return HB_OK;
    }
    if (opcode == 0x99) {
        /* CDQ / CWDE / CQO — Sign-Extend EAX into EDX:EAX (CDQ) for 32-bit
         * opsize, sign-extend AX into DX:EAX (CWD) for 16-bit opsize, or
         * sign-extend RAX into RDX:RAX (CQO) for 64-bit opsize. HyperBridge
         * names the i386 default op as CWD (matches the existing x64 naming);
         * the lift reads op1.size to choose. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_CWD;
        out->reads_flags = false;
        out->writes_flags = false;
        set_imm(out, 1, operand16 ? 2 : 4, 1);
        return HB_OK;
    }
    if (opcode == 0xCF) {
        /* IRET / IRETD — interrupt return. 16-bit opsize = IRET (pops IP+CS+FLAGS),
         * 32-bit opsize = IRETD (pops EIP+CS+EFLAGS). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_IRET;
        out->reads_flags = true;
        out->writes_flags = true;
        out->is_ret = true;
        out->stack_delta = -(int)(operand16 ? 6 : 12);
        return HB_OK;
    }
    if (opcode == 0xCE) {
        /* INTO — interrupt 4 if OF=1. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_INTO;
        out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0xCC) {
        /* INT3 — debug breakpoint (3-byte form, also reachable as INT 3). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_INT3;
        return HB_OK;
    }
    if (opcode == 0xCD) {
        /* INT n — vectored software interrupt. imm8 vector. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t vector = read_u8(d);
        out->opcode = HB_INS_INT;
        out->reads_flags = true;
        out->writes_flags = true;
        set_imm(out, 1, vector, 1);
        return HB_OK;
    }
    if (opcode == 0xF1) {
        /* INT1 / ICEBP — debug breakpoint (1-byte form, ICEBP). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_INT1;
        return HB_OK;
    }
    if (opcode == 0xCA) {
        /* RETF imm16 — far return, with stack-adjust. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
        uint16_t imm = (uint16_t)read_s16(d);
        out->opcode = HB_INS_RETF;
        out->is_ret = true;
        out->ret_imm = imm;
        out->writes_flags = true;
        out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0xCB) {
        /* RETF — far return, no stack-adjust. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_RETF;
        out->is_ret = true;
        out->writes_flags = true;
        out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0x9A) {
        /* CALL ptr16:32 — far direct call. Imm32 offset + imm16 segment. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 4 + 2)) return HB_ERR_DECODE_FAILED;
        uint32_t offset = (uint32_t)read_s32(d);
        uint16_t segment = (uint16_t)read_s16(d);
        out->opcode = HB_INS_CALL;
        out->is_call = true;
        out->writes_flags = true;
        out->reads_flags = true;
        out->branch_target = offset;
        /* Encode the segment in op1.imm, offset in op2.imm. */
        set_imm(out, 1, segment, 2);
        set_imm(out, 2, (int64_t)(int32_t)offset, 4);
        return HB_OK;
    }
    if (opcode == 0xEA) {
        /* JMP ptr16:32 — far direct jump. Imm32 offset + imm16 segment. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 4 + 2)) return HB_ERR_DECODE_FAILED;
        uint32_t offset = (uint32_t)read_s32(d);
        uint16_t segment = (uint16_t)read_s16(d);
        out->opcode = HB_INS_JMP;
        out->writes_flags = true;
        out->branch_target = offset;
        set_imm(out, 1, segment, 2);
        set_imm(out, 2, (int64_t)(int32_t)offset, 4);
        return HB_OK;
    }
    if (opcode == 0xD6) {
        /* SALC (Set AL on Carry, undocumented opcode 0xD6). Implemented as
         * MOV AL, 0xFF if CF=1 else 0x00. We decode it and let the lift
         * materialize the SETC semantics. The 0x66 prefix is silently
         * ignored for this 8-bit-only opcode (Capstone accepts it). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_SALC;
        out->reads_flags = true;
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xD7) {
        /* XLATB / XLAT — AL = [EBX+AL] (or [BX+AL] with 0x67). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_XLAT;
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xC8) {
        /* ENTER imm16, imm8 — make stack frame for procedure parameters. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 3)) return HB_ERR_DECODE_FAILED;
        uint16_t frame_size = (uint16_t)read_s16(d);
        uint8_t nesting = read_u8(d);
        out->opcode = HB_INS_ENTER;
        out->writes_flags = true;
        out->reads_flags = true;
        set_imm(out, 1, frame_size, 2);
        set_imm(out, 2, nesting, 1);
        return HB_OK;
    }
    if (opcode >= 0x6C && opcode <= 0x6F) {
        /* INS/OUTS — string port I/O (privileged; #GP unless CPL<=IOPL).
         * Decoded as IN/OUT for the lift/interpret to raise a fault; op size
         * tracks operand16 (16-bit for 0x66 on INSW, 8-bit for INSB, 32-bit
         * for INSD, ditto for OUTS). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (opcode == 0x6C) out->opcode = HB_INS_INS;
        else if (opcode == 0x6D) out->opcode = HB_INS_INS;
        else if (opcode == 0x6E) out->opcode = HB_INS_OUTS;
        else out->opcode = HB_INS_OUTS;
        if (opcode == 0x6C) set_imm(out, 1, 1, 1);
        else if (opcode == 0x6D) set_imm(out, 1, operand16 ? 2 : 4, 1);
        else if (opcode == 0x6E) set_imm(out, 1, 1, 1);
        else set_imm(out, 1, operand16 ? 2 : 4, 1);
        out->reads_flags = (opcode >= 0x6E);
        out->writes_flags = (opcode < 0x6E);
        return HB_OK;
    }
    if (opcode == 0xE4 || opcode == 0xE5 || opcode == 0xEC || opcode == 0xED) {
        /* IN — port I/O (privileged). HyperBridge doesn't model it; raise fault
         * rather than silently producing wrong state. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_IN;
        out->writes_flags = true;
        if (opcode == 0xE4 || opcode == 0xE5) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t port = read_u8(d);
            set_imm(out, 1, port, 1);
        }
        return HB_OK;
    }
    if (opcode == 0xE6 || opcode == 0xE7 || opcode == 0xEE || opcode == 0xEF) {
        /* OUT — port I/O (privileged). HyperBridge doesn't model it. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_OUT;
        out->reads_flags = true;
        if (opcode == 0xE6 || opcode == 0xE7) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t port = read_u8(d);
            set_imm(out, 1, port, 1);
        }
        return HB_OK;
    }
    if (opcode == 0xF4) {
        /* HLT — privileged. HyperBridge raises a fault (HB_ERR_EXEC_FAULT or
         * UNSUPPORTED_FEATURE) so the user knows the binary did something it
         * shouldn't. We do not silently NOP this. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_HLT;
        return HB_OK;
    }
    if (opcode == 0x27) {
        /* DAA — Decimal Adjust AL after Addition. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_DAA;
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0x2F) {
        /* DAS — Decimal Adjust AL after Subtraction. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_DAS;
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0x37) {
        /* AAA — ASCII Adjust After Addition. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_AAA;
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0x3F) {
        /* AAS — ASCII Adjust After Subtraction. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_AAS;
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xD4) {
        /* AAM — ASCII Adjust After Multiply. imm8 (base) is mandatory. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t base = read_u8(d);
        out->opcode = HB_INS_AAM;
        out->reads_flags = false;
        out->writes_flags = true;
        set_imm(out, 1, base, 1);
        return HB_OK;
    }
    if (opcode == 0xD5) {
        /* AAD — ASCII Adjust Before Division. imm8 (base) is mandatory. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t base = read_u8(d);
        out->opcode = HB_INS_AAD;
        out->reads_flags = false;
        out->writes_flags = true;
        set_imm(out, 1, base, 1);
        return HB_OK;
    }
    if (opcode == 0x62) {
        /* BOUND r16/32, m16/32&16/32 — array bounds check. 0x62 is also the EVEX
         * prefix in 64-bit mode; in 32-bit mode (this decoder) it is BOUND only. */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_BOUND;
        out->writes_flags = false;
        /* dst is the register; the memory operand is encoded as a 2-elem mBOUND
         * (low, high). The lift expects dst.size to be the index size and src1
         * to be a normal m16/32. parse_modrm_ext produces a single-element mem
         * operand; the interpreter reads 2 elements from that address. */
        return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
    }
    if (opcode == 0x63) {
        /* ARPL r/m16, r16 — Adjust RPL Field of Segment Selector (in 32-bit mode;
         * MOVSXD in 64-bit mode, which this decoder does not handle). */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_ARPL;
        out->writes_flags = true;
        return parse_modrm(d, modrm, 2, out, 1, 2, false);
    }
    if (opcode == 0xC5) {
        /* LDS r16/32, m16:32 — Load Far Pointer. (In 64-bit mode, 0xC5 is the
         * VEX 2-byte prefix, so this is i386-only.) */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LDS;
        out->writes_flags = false;
        /* op1 is the destination GPR, op2 is the m48 (offset:selector) memory. */
        return parse_modrm(d, modrm, 6, out, 1, 2, false);
    }
    if (opcode == 0xC4) {
        /* LES r16/32, m16:32 — Load Far Pointer. (In 64-bit mode, 0xC4 is the
         * VEX 3-byte prefix, so this is i386-only.) */
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LES;
        out->writes_flags = false;
        return parse_modrm(d, modrm, 6, out, 1, 2, false);
    }
    if (opcode == 0x86 || opcode == 0x87) {
        /* XCHG r/m8,r8 and r/m16/32,r16/32.  LOCK is consumed by the
           prefix scanner; reject prefixes this decoder cannot model. */
        if (prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (address16 && !can_read(d, 1)) return HB_ERR_UNSUPPORTED_OPCODE;
        if (address16 && (d->code[d->pos] >> 6) != 3) return HB_ERR_UNSUPPORTED_OPCODE;
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
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (address16 && !can_read(d, 1)) return HB_ERR_UNSUPPORTED_OPCODE;
        if (address16 && (d->code[d->pos] >> 6) != 3) return HB_ERR_UNSUPPORTED_OPCODE;
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
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_XCHG;
        out->writes_flags = false;
        set_reg(out, 1, HB_REG_RAX, operand16 ? 2 : 4);
        set_reg(out, 2, opcode & 7, operand16 ? 2 : 4);
        return HB_OK;
    }
    if (opcode == 0x9C || opcode == 0x9D) {
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = opcode == 0x9C ? HB_INS_PUSHF : HB_INS_POPF;
        out->writes_flags = opcode == 0x9D;
        out->stack_delta = operand16 ? 2 : 4;
        set_imm(out, 1, 0, operand16 ? 2 : 4);
        return HB_OK;
    }
    if (opcode == 0x9F) {
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LAHF;
        out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0x9E) {
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_SAHF;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xA4 || opcode == 0xA5) {
        /* MOVS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOVS;
        set_reg(out, 1, HB_REG_RSI, opcode == 0xA4 ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xA6 || opcode == 0xA7) {
        /* CMPS m8/m16/m32. REPNE/REPE are carried as an immediate mode. */
        if (prefix_f0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_CMPS;
        set_reg(out, 1, HB_REG_RSI, opcode == 0xA6 ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAC || opcode == 0xAD) {
        /* LODS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_LODS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAC ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xAE || opcode == 0xAF) {
        /* SCAS m8/m16/m32. REPNE/REPE are carried as an immediate mode. */
        if (prefix_f0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_SCAS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAE ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAA || opcode == 0xAB) {
        /* STOS m8/m16/m32. REP is carried as an immediate mode. */
        if (prefix_f0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_STOS;
        set_reg(out, 1, HB_REG_RAX, opcode == 0xAA ? 1 : (operand16 ? 2 : 4));
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }

    if (opcode == 0x0F) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (((op2 == 0x2B) && !prefix_f2 && !prefix_f3) ||
            ((op2 == 0xE7) && operand16 && !prefix_f2 && !prefix_f3)) {
            /* MOVNTPS/MOVNTPD/MOVNTDQ: decode as the same 128-bit store IR as
             * regular packed SSE moves.  The non-temporal cache hint does not
             * change guest-visible state; register ModRM forms are invalid. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, true);
            if (r != HB_OK) return r;
            mark_xmm_operands(out);
            return HB_OK;
        }
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
        if (op2 == 0xAE) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t ext = (modrm >> 3) & 7;
            if (mod == 3) {
                if ((modrm & 7) == 0 && (ext == 5 || ext == 6 || ext == 7)) {
                    out->opcode = HB_INS_FENCE;
                    set_imm(out, 1, ext == 5 ? HB_FENCE_ACQUIRE :
                                    ext == 6 ? HB_FENCE_FULL : HB_FENCE_RELEASE, 1);
                    return HB_OK;
                }
                return HB_ERR_UNSUPPORTED_OPCODE;
            }
            if (ext == 0) {
                out->opcode = HB_INS_X87_FXSAVE;
                return parse_modrm_ext(d, modrm, HB_SIZE_512, out, 1);
            }
            if (ext == 1) {
                out->opcode = HB_INS_X87_FXRSTOR;
                return parse_modrm_ext(d, modrm, HB_SIZE_512, out, 1);
            }
            if (ext == 2) {
                out->opcode = HB_INS_NOP;
                return parse_modrm_ext(d, modrm, 4, out, 1);
            }
            if (ext == 3) {
                hb_result_t r;
                out->opcode = HB_INS_MOV;
                r = parse_modrm_ext(d, modrm, 4, out, 1);
                if (r != HB_OK) return r;
                set_imm(out, 2, HB_X86_DEFAULT_MXCSR, 4);
                return HB_OK;
            }
            if (ext == 7) {
                out->opcode = HB_INS_NOP;
                return parse_modrm_ext(d, modrm, 1, out, 1);
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
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
        if (opcode == 0x0F && !prefix_f2 && !prefix_f3) {
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
        if (op2 == 0xC6 && !prefix_f2 && !prefix_f3) {
            /* SHUFPS/SHUFPD xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_SHUFPD : HB_INS_SHUFPS;
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
        if (op2 == 0x38) {
            /* F2/F3 0F 38 — CRC32 (F2 only), ADCX (66 only). The dispatch is
             * the same shape as the no-prefix 0F 38 path: ssse3_0f38_opcode
             * and sse41_0f38_opcode lookups. For F2 we accept CRC32 (F0/F1);
             * for F3 we accept ADOX (F7). For unrecognized F2/F3 0F 38 forms
             * we fall through to the next 0F 38 block (no prefix). */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            if (prefix_f2) {
                if (op3 == 0xf0 || op3 == 0xf1) {
                    /* CRC32 r32, r/m8 / CRC32 r32, r/m32. */
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_CRC32;
                    out->writes_flags = true;
                    hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    /* Mark src as r/m8 (CRC32 F0) or r/m32 (CRC32 F1). */
                    if (op3 == 0xf0 && out->op2.is_reg) {
                        uint8_t ro = 0;
                        out->op2.reg = reg8_idx(out->op2.reg, &ro);
                        out->op2.reg_offset = ro;
                        out->op2.size = 1;
                    } else if (op3 == 0xf0) {
                        out->op2.size = 1;
                    }
                    return HB_OK;
                }
            }
            if (prefix_f3) {
                if (op3 == 0xf6) {
                    /* F3 0F 38 F6 = ADOX r32, r/m32. */
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_ADOX;
                    out->writes_flags = true;
                    return parse_modrm(d, modrm, 4, out, 1, 2, false);
                }
            }
            /* Unrecognized F2/F3 0F 38: fall through to no-prefix dispatch. */
            d->pos--;
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (op2 == 0x3A) {
            /* F2/F3 0F 3A: no defined forms (SSE4.1 imm8 + AESKEYGENASSIST
             * use 66 prefix, not F2/F3). We reject. */
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode == 0x0F) {
        size_t saved_pos = d->pos;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t op2 = read_u8(d);
        if (op2 == 0x38) {
            /* 0F38: SSSE3 (no-prefix = MMX, 0x66 = SSE), SSE4.1, AES, GF2P8MULB. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            int vec_opcode = ssse3_0f38_opcode(op3);
            if (!vec_opcode) vec_opcode = sse41_0f38_opcode(op3);
            if (vec_opcode) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = vec_opcode;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                return HB_OK;
            }
            /* Unrecognized 0F38 opcode: decode as a generic VEC. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_VEC;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x3A) {
            /* 0F3A: SSE4.1 imm8 + AES + PCLMULQDQ (all SSE = 0x66 prefix). */
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            uint8_t modrm = read_u8(d);
            int vec_opcode = sse41_0f3a_opcode(op3);
            out->opcode = vec_opcode ? vec_opcode : HB_INS_VEC;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
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
        if (operand16 && (op2 == 0xD5 || op2 == 0xE5 || op2 == 0xE4 || op2 == 0xF5)) {
            /* SSE2 packed 16-bit multiply family, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xD5) out->opcode = HB_INS_PMULLW;
            else if (op2 == 0xE5) out->opcode = HB_INS_PMULHW;
            else if (op2 == 0xE4) out->opcode = HB_INS_PMULHUW;
            else out->opcode = HB_INS_PMADDWD;
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
        if (op2 == 0xC6 && !prefix_f2 && !prefix_f3) {
            /* SHUFPS/SHUFPD xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_SHUFPD : HB_INS_SHUFPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
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
        if ((!operand16 && !prefix_f2 && !prefix_f3 && op2 == 0x50) ||
            (operand16 && op2 == 0x50)) {
            /* MOVMSKPS/MOVMSKPD r32, xmm: extract packed FP sign bits. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_MOVMSKPD : HB_INS_MOVMSKPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (!out->op2.is_reg) return HB_ERR_UNSUPPORTED_OPCODE;
            mark_xmm_operand(out, 2);
            out->op1.size = 4;
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
        if (!prefix_f2 && !prefix_f3 && (op2 == 0x12 || op2 == 0x13 || op2 == 0x16 || op2 == 0x17)) {
            /* MOVLPS/MOVLPD and MOVHPS/MOVHPD memory forms move one qword lane.
             * Register 0F 12/16 aliases MOVHLPS/MOVLHPS.
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if ((modrm >> 6) == 3) {
                if (operand16 || op2 == 0x13 || op2 == 0x17) return HB_ERR_UNSUPPORTED_OPCODE;
                out->opcode = op2 == 0x12 ? HB_INS_MOVHLPS : HB_INS_MOVLHPS;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                return HB_OK;
            }
            if (op2 == 0x16 || op2 == 0x17) {
                out->opcode = operand16 ? HB_INS_MOVHPD : HB_INS_MOVHPS;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, 8, out, 1, 2, op2 == 0x17);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, op2 == 0x17 ? 2 : 1);
                if (op2 == 0x17) out->op1.size = 8;
                else out->op2.size = 8;
                return HB_OK;
            }
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
        if ((op2 == 0x59 || op2 == 0x5E) && !prefix_f2 && !prefix_f3) {
            /* Packed MUL/DIV: MULPS/DIVPS and MULPD/DIVPD. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (operand16) out->opcode = (op2 == 0x59) ? HB_INS_MULPD : HB_INS_DIVPD;
            else out->opcode = (op2 == 0x59) ? HB_INS_MULPS : HB_INS_DIVPS;
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
        if (op2 == 0x51) {
            /* SQRTPS/SQRTPD/SQRTSS/SQRTSD xmm, xmm/mem. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 16;
            if (prefix_f2) { out->opcode = HB_INS_SQRTSD; mem_size = 8; }
            else if (prefix_f3) { out->opcode = HB_INS_SQRTSS; mem_size = 4; }
            else if (operand16) out->opcode = HB_INS_SQRTPD;
            else out->opcode = HB_INS_SQRTPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if ((op2 == 0x52 || op2 == 0x53) && !operand16 && !prefix_f2) {
            /* RSQRTPS/RSQRTSS/RCPPS/RCPSS xmm, xmm/mem. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = prefix_f3 ? 4 : 16;
            if (op2 == 0x52) out->opcode = prefix_f3 ? HB_INS_RSQRTSS : HB_INS_RSQRTPS;
            else out->opcode = prefix_f3 ? HB_INS_RCPSS : HB_INS_RCPPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
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

    /* Per Intel SDM Vol 2: when the form is register-only (ModRM.mod == 3), the
     * 0x67 address-size override prefix is silently ignored. We also let 0x67
     * through to the dispatcher for the shift/rotate groups (C0/C1/D0-D3)
     * since the modrm parser will produce the correct base/index/width even
     * with 16-bit addressing. Other 0x67 forms in 32-bit mode are still
     * rejected. */
    bool suppress_addr16 = false;
    if (address16 && can_read(d, 1)) {
        uint8_t peek = d->code[d->pos];
        if ((peek >> 6) == 3) suppress_addr16 = true;
    }
    if ((operand16 && !((opcode <= 0x3D) && ((opcode & 7) <= 5)) &&
          !(opcode >= 0x50 && opcode <= 0x5F) &&
          !(opcode >= 0x70 && opcode <= 0x7F) &&
          !(opcode >= 0x88 && opcode <= 0x8B) && opcode != 0x8D &&
          opcode != 0x0F &&
          !(opcode >= 0xA0 && opcode <= 0xA3) && !(opcode >= 0xB0 && opcode <= 0xBF) &&
          !(opcode >= 0x40 && opcode <= 0x4F) &&
          opcode != 0x68 && opcode != 0x6A &&
          opcode != 0x8F && opcode != 0xC2 && opcode != 0xC3 &&
          opcode != 0x9C && opcode != 0x9D && opcode != 0x9E && opcode != 0x9F &&
          opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
          opcode != 0x84 && opcode != 0x85 && opcode != 0xA8 && opcode != 0xA9 &&
          opcode != 0xF6 && opcode != 0xF7 &&
          opcode != 0x69 && opcode != 0x6B &&
          opcode != 0xC0 && opcode != 0xC1 &&
          opcode != 0xFE && opcode != 0xFF &&
          !(opcode >= 0xD0 && opcode <= 0xD3) &&
          opcode != 0xD6 && opcode != 0xD7 &&
          opcode != 0xC6 && opcode != 0xC7 && opcode != 0xC9 &&
          !(opcode >= 0xE0 && opcode <= 0xE3) &&
          opcode != 0xE8 && opcode != 0xE9 && opcode != 0xEB &&
          !(opcode >= 0xD8 && opcode <= 0xDF) &&
          !(opcode >= 0x6C && opcode <= 0x6F)) ||
        (address16 && !suppress_addr16 &&
                       opcode != 0xC0 && opcode != 0xC1 &&
                       !(opcode >= 0xD0 && opcode <= 0xD3) &&
                       opcode != 0x0F) ||
        (prefix_f2 && opcode != 0x90 && opcode != 0xC3 && opcode != 0xC2 &&
                     !(opcode >= 0x6C && opcode <= 0x6F)) ||
        (prefix_f3 && opcode != 0x90 && opcode != 0xC3 && opcode != 0xC2 &&
                     !(opcode >= 0x6C && opcode <= 0x6F))) {
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
        out->stack_delta = -(int)(operand16 ? 2 : 4);
        set_reg(out, 1, opcode & 7, operand16 ? 2 : 4);
        return HB_OK;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        out->opcode = HB_INS_POP;
        out->stack_delta = operand16 ? 2 : 4;
        set_reg(out, 1, opcode & 7, operand16 ? 2 : 4);
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
        out->stack_delta = -(int)(operand16 ? 2 : 4);
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        if (operand16) {
            int16_t v = (int16_t)read_s16(d);
            set_imm(out, 1, v, 2);
        } else {
            set_imm(out, 1, (int64_t)read_s32(d), 4);
        }
        return HB_OK;
    }
    if (opcode == 0x6A) {
        out->opcode = HB_INS_PUSH;
        out->stack_delta = -(int)(operand16 ? 2 : 4);
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t v = read_s8(d);
        set_imm(out, 1, v, operand16 ? 2 : 4);
        return HB_OK;
    }

    /* Group: INC reg (0x40-0x47) -- in x86 these are real INC, not REX */
    if (opcode >= 0x40 && opcode <= 0x47) {
        out->opcode = HB_INS_INC;
        out->writes_flags = true;
        set_reg(out, 1, opcode & 7, operand16 ? 2 : 4);
        return HB_OK;
    }
    /* Group: DEC reg (0x48-0x4F) -- in x86 these are real DEC, not REX */
    if (opcode >= 0x48 && opcode <= 0x4F) {
        out->opcode = HB_INS_DEC;
        out->writes_flags = true;
        set_reg(out, 1, opcode & 7, operand16 ? 2 : 4);
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
    if (opcode >= 0xE0 && opcode <= 0xE3) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        int8_t rel = read_s8(d);
        /* 0xE3 is LOOPNE in 64-bit mode but JECXZ/JCXZ in 32-bit. For
         * i386-only decode we use HB_INS_JRCXZ; the lift/interpret key off
         * op1.size (4 = ECX in 32-bit, 2 = CX in 16-bit). */
        out->opcode = opcode == 0xE3 ? HB_INS_JRCXZ : HB_INS_LOOP;
        out->is_branch = true;
        out->is_conditional = true;
        out->branch_target = addr + d->pos + rel;
        out->reads_flags = opcode == 0xE0 || opcode == 0xE1;
        set_reg(out, 1, HB_REG_RCX, address16 ? 2 : 4);
        set_imm(out, 2, opcode - 0xE0, 1);
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
        if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
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
        if (op2 == 0x00) {
            /* Group 6: SLDT/STR/LLDT/LTR/VERR/VERW r/m16/32.
             * Decoded as MOV_SEG-shaped for the lift; privileged forms
             * (LLDT/LTR) are still allowed through — the runtime will #GP. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOV_SEG;
            out->writes_flags = false;
            return parse_modrm_ext(d, modrm, operand16 ? 2 : 4, out, 1);
        }
        if (op2 == 0x01) {
            /* Group 7: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG. Privileged in i386
             * user mode, but the encoding is well-defined. Decoded as
             * MOV-shaped; the runtime raises #GP for the privileged ones. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOV_SEG;
            out->writes_flags = false;
            return parse_modrm_ext(d, modrm, operand16 ? 2 : 4, out, 1);
        }
        if (op2 == 0x02) {
            /* LAR r16/32, r/m16/32 — load access rights. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOV;
            out->writes_flags = true;
            return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
        }
        if (op2 == 0x03) {
            /* LSL r16/32, r/m16/32 — load segment limit. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MOV;
            out->writes_flags = true;
            return parse_modrm(d, modrm, operand16 ? 2 : 4, out, 1, 2, false);
        }
        if (op2 == 0x05) {
            /* SYSCALL — only valid in 64-bit mode. In 32-bit i386 this is #UD. */
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (op2 == 0x06) {
            /* CLTS — clear task-switched flag, privileged. */
            return HB_ERR_UNSUPPORTED_OPCODE;
        }
        if (op2 == 0xA0 || op2 == 0xA1 || op2 == 0xA8 || op2 == 0xA9) {
            /* PUSH/POP FS/GS (0F-escape form). Valid in 32-bit user mode; #UD in
             * 64-bit mode (we never get here in that mode anyway). LOCK/REP/REPNE
             * are illegal prefixes here. */
            if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
            bool is_push = (op2 == 0xA0 || op2 == 0xA8);
            int seg = (op2 == 0xA0 || op2 == 0xA1) ? 4 /* FS */ : 5 /* GS */;
            out->opcode = is_push ? HB_INS_PUSH_SEG : HB_INS_POP_SEG;
            out->writes_flags = false;
            out->stack_delta = is_push ? (operand16 ? 2 : 4) : -(int)(operand16 ? 2 : 4);
            set_imm(out, 1, 0, operand16 ? 2 : 4);
            set_imm(out, 2, seg, 1);
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
        if (op2 == 0xB4 || op2 == 0xB5) {
            /* LFS / LGS r16/32, m16:32. 0F B4=LFS, 0F B5=LGS. */
            if (prefix_f0 || prefix_f2 || prefix_f3) return HB_ERR_UNSUPPORTED_OPCODE;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = (op2 == 0xB4) ? HB_INS_LFS : HB_INS_LGS;
            out->writes_flags = false;
            return parse_modrm(d, modrm, 6, out, 1, 2, false);
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
            /* BSF/BSR r16/32, r/m16/32. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xBC) ? HB_INS_BSF : HB_INS_BSR;
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
        if (op2 == 0x20 || op2 == 0x21 || op2 == 0x22 || op2 == 0x23) {
            /* MOV r32, CRn/DRn (0F 20/21 read) and MOV CRn/DRn, r32 (0F 22/23 write).
             * Privileged in 32-bit user mode; runtime raises #GP. We decode as
             * MOV_CR/MOV_DR; the lift/codegen route the privileged form. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            bool write = (op2 == 0x22 || op2 == 0x23);
            out->opcode = (op2 == 0x20 || op2 == 0x22) ? HB_INS_MOV_CR : HB_INS_MOV_DR;
            out->writes_flags = false;
            hb_result_t r = parse_modrm_ext(d, modrm, 4, out, 1);
            if (r != HB_OK) return r;
            if (write) {
                /* For write form (0F 22/23), ModRM.reg = CRn/DRn index and
                 * ModRM.rm = r32. parse_modrm_ext gives r32 in op1, r/m in op2;
                 * but for write the source is r32 and the destination is the
                 * control register. Swap so dst = CRn/DRn, src = r32. */
                __typeof__(out->op1) saved = out->op1;
                out->op1 = out->op2;
                out->op2 = saved;
            }
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL; /* SAL alias for SHL (Intel SDM) */
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL; /* SAL alias for SHL (Intel SDM) */
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL; /* SAL alias for SHL (Intel SDM) */
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
