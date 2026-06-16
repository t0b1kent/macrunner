#include "hb_decoder.h"
#include "hb_ir.h"
#include <string.h>
#include <stdlib.h>

#define HB_X64_DEFAULT_MXCSR 0x1f80u
#define HB_X87_DEFAULT_CONTROL_WORD 0x037fu

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

static inline void replace_reg_index(hb_decoded_t* out, int slot, int r) {
    if (slot == 1 && out->op1.is_reg) out->op1.reg = r;
    else if (slot == 2 && out->op2.is_reg) out->op2.reg = r;
    else if (slot == 3 && out->op3.is_reg) out->op3.reg = r;
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

static inline void set_extra_imm8(hb_decoded_t* out, uint8_t v) {
    out->has_imm8 = true;
    out->imm8 = v;
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

static inline void mark_vec_operand(hb_decoded_t* out, int slot, uint8_t bytes) {
    mark_xmm_operand(out, slot);
    if (slot == 1 && out->op1.is_reg) out->op1.size = bytes;
    else if (slot == 2 && out->op2.is_reg) out->op2.size = bytes;
    else if (slot == 3 && out->op3.is_reg) out->op3.size = bytes;
}

static inline void mark_vec_operands(hb_decoded_t* out, uint8_t bytes) {
    mark_vec_operand(out, 1, bytes);
    mark_vec_operand(out, 2, bytes);
    mark_vec_operand(out, 3, bytes);
    if (out->op1.is_mem) out->op1.size = bytes;
    if (out->op2.is_mem) out->op2.size = bytes;
    if (out->op3.is_mem) out->op3.size = bytes;
}

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

static int sse2_0f_packed_opcode(uint8_t op) {
    switch (op) {
        case 0xc4: return HB_INS_PINSRW;
        case 0xc5: return HB_INS_PEXTRW;
        case 0xd1: return HB_INS_PSRLW;
        case 0xd2: return HB_INS_PSRLD;
        case 0xd3: return HB_INS_PSRLQ;
        case 0xd5: return HB_INS_PMULLW;
        case 0xd8: return HB_INS_PSUBUSB;
        case 0xd9: return HB_INS_PSUBUSW;
        case 0xda: return HB_INS_PMINUB;
        case 0xdc: return HB_INS_PADDUSB;
        case 0xdd: return HB_INS_PADDUSW;
        case 0xde: return HB_INS_PMAXUB;
        case 0xe1: return HB_INS_PSRAW;
        case 0xe2: return HB_INS_PSRAD;
        case 0xe0: return HB_INS_PAVGB;
        case 0xe3: return HB_INS_PAVGW;
        case 0xe4: return HB_INS_PMULHUW;
        case 0xe5: return HB_INS_PMULHW;
        case 0xe8: return HB_INS_PSUBSB;
        case 0xe9: return HB_INS_PSUBSW;
        case 0xea: return HB_INS_PMINSW;
        case 0xec: return HB_INS_PADDSB;
        case 0xed: return HB_INS_PADDSW;
        case 0xee: return HB_INS_PMAXSW;
        case 0xf1: return HB_INS_PSLLW;
        case 0xf2: return HB_INS_PSLLD;
        case 0xf3: return HB_INS_PSLLQ;
        case 0xf4: return HB_INS_PMULUDQ;
        case 0xf5: return HB_INS_PMADDWD;
        case 0xf6: return HB_INS_PSADBW;
        default: return 0;
    }
}

static int sse2_0f_compare_opcode(uint8_t op) {
    switch (op) {
        case 0x64: return HB_INS_PCMPGTB;
        case 0x65: return HB_INS_PCMPGTW;
        case 0x66: return HB_INS_PCMPGTD;
        case 0x74: return HB_INS_PCMPEQB;
        case 0x75: return HB_INS_PCMPEQW;
        case 0x76: return HB_INS_PCMPEQD;
        default: return 0;
    }
}

static int sse2_0f_unpack_pack_opcode(uint8_t op) {
    switch (op) {
        case 0x60: return HB_INS_PUNPCKLBW;
        case 0x61: return HB_INS_PUNPCKLWD;
        case 0x62: return HB_INS_PUNPCKLDQ;
        case 0x63: return HB_INS_PACKSSWB;
        case 0x67: return HB_INS_PACKUSWB;
        case 0x68: return HB_INS_PUNPCKHBW;
        case 0x69: return HB_INS_PUNPCKHWD;
        case 0x6a: return HB_INS_PUNPCKHDQ;
        case 0x6b: return HB_INS_PACKSSDW;
        case 0x6c: return HB_INS_PUNPCKLQDQ;
        case 0x6d: return HB_INS_PUNPCKHQDQ;
        default: return 0;
    }
}

static bool sse2_0f_variable_shift_opcode(uint8_t op) {
    return op == 0xd1 || op == 0xd2 || op == 0xd3 ||
           op == 0xe1 || op == 0xe2 ||
           op == 0xf1 || op == 0xf2 || op == 0xf3;
}

static uint8_t evex_packed_int_mask_lane(int ins) {
    switch (ins) {
        case HB_INS_PUNPCKLBW: case HB_INS_PUNPCKHBW:
        case HB_INS_PACKSSWB: case HB_INS_PACKUSWB:
        case HB_INS_PADDB: case HB_INS_PSUBB:
        case HB_INS_PSUBUSB: case HB_INS_PSUBSB:
        case HB_INS_PMINUB: case HB_INS_PMAXUB:
        case HB_INS_PADDUSB: case HB_INS_PADDSB:
        case HB_INS_PAVGB: case HB_INS_PSHUFB:
        case HB_INS_PSIGNB: case HB_INS_PABSB:
            return 1;
        case HB_INS_PUNPCKLWD: case HB_INS_PUNPCKHWD:
        case HB_INS_PACKSSDW:
        case HB_INS_PADDW: case HB_INS_PSUBW:
        case HB_INS_PMULLW: case HB_INS_PSUBUSW:
        case HB_INS_PSUBSW: case HB_INS_PMINSW:
        case HB_INS_PADDUSW: case HB_INS_PADDSW:
        case HB_INS_PMAXSW: case HB_INS_PAVGW:
        case HB_INS_PMULHUW: case HB_INS_PMULHW:
        case HB_INS_PMADDUBSW: case HB_INS_PMULHRSW:
        case HB_INS_PSIGNW: case HB_INS_PABSW:
            return 2;
        case HB_INS_PUNPCKLDQ: case HB_INS_PUNPCKHDQ:
        case HB_INS_PADDD: case HB_INS_PSUBD:
        case HB_INS_PMADDWD: case HB_INS_PMULLD:
        case HB_INS_PSIGND: case HB_INS_PABSD:
            return 4;
        case HB_INS_PUNPCKLQDQ: case HB_INS_PUNPCKHQDQ:
        case HB_INS_PADDQ: case HB_INS_PSUBQ:
        case HB_INS_PMULUDQ: case HB_INS_PSADBW:
            return 8;
        default:
            return 0;
    }
}

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
        case 0xdb: return HB_INS_AESIMC;
        case 0xdc: return HB_INS_AESENC;
        case 0xdd: return HB_INS_AESENCLAST;
        case 0xde: return HB_INS_AESDEC;
        case 0xdf: return HB_INS_AESDECLAST;
        default: return 0;
    }
}

static int sse41_0f3a_opcode(uint8_t op) {
    switch (op) {
        case 0x0c: return HB_INS_BLENDPS;
        case 0x0d: return HB_INS_BLENDPD;
        case 0x0e: return HB_INS_PBLENDW;
        case 0x0f: return HB_INS_PALIGNR;
        case 0x40: return HB_INS_DPPS;
        case 0x41: return HB_INS_DPPD;
        case 0x42: return HB_INS_MPSADBW;
        case 0x44: return HB_INS_PCLMULQDQ;
        case 0x60: return HB_INS_PCMPESTRM;
        case 0x61: return HB_INS_PCMPESTRI;
        case 0x62: return HB_INS_PCMPISTRM;
        case 0x63: return HB_INS_PCMPISTRI;
        case 0xdf: return HB_INS_AESKEYGENASSIST;
        default: return 0;
    }
}

static int fma3_0f38_opcode(uint8_t op, bool vex_w) {
    switch (op) {
        case 0x96: return vex_w ? HB_INS_VFMADDSUB132PD : HB_INS_VFMADDSUB132PS;
        case 0x97: return vex_w ? HB_INS_VFMSUBADD132PD : HB_INS_VFMSUBADD132PS;
        case 0x98: return vex_w ? HB_INS_VFMADD132PD : HB_INS_VFMADD132PS;
        case 0x99: return vex_w ? HB_INS_VFMADD132SD : HB_INS_VFMADD132SS;
        case 0x9a: return vex_w ? HB_INS_VFMSUB132PD : HB_INS_VFMSUB132PS;
        case 0x9b: return vex_w ? HB_INS_VFMSUB132SD : HB_INS_VFMSUB132SS;
        case 0x9c: return vex_w ? HB_INS_VFNMADD132PD : HB_INS_VFNMADD132PS;
        case 0x9d: return vex_w ? HB_INS_VFNMADD132SD : HB_INS_VFNMADD132SS;
        case 0x9e: return vex_w ? HB_INS_VFNMSUB132PD : HB_INS_VFNMSUB132PS;
        case 0x9f: return vex_w ? HB_INS_VFNMSUB132SD : HB_INS_VFNMSUB132SS;
        case 0xa6: return vex_w ? HB_INS_VFMADDSUB213PD : HB_INS_VFMADDSUB213PS;
        case 0xa7: return vex_w ? HB_INS_VFMSUBADD213PD : HB_INS_VFMSUBADD213PS;
        case 0xa8: return vex_w ? HB_INS_VFMADD213PD : HB_INS_VFMADD213PS;
        case 0xa9: return vex_w ? HB_INS_VFMADD213SD : HB_INS_VFMADD213SS;
        case 0xaa: return vex_w ? HB_INS_VFMSUB213PD : HB_INS_VFMSUB213PS;
        case 0xab: return vex_w ? HB_INS_VFMSUB213SD : HB_INS_VFMSUB213SS;
        case 0xac: return vex_w ? HB_INS_VFNMADD213PD : HB_INS_VFNMADD213PS;
        case 0xad: return vex_w ? HB_INS_VFNMADD213SD : HB_INS_VFNMADD213SS;
        case 0xae: return vex_w ? HB_INS_VFNMSUB213PD : HB_INS_VFNMSUB213PS;
        case 0xaf: return vex_w ? HB_INS_VFNMSUB213SD : HB_INS_VFNMSUB213SS;
        case 0xb6: return vex_w ? HB_INS_VFMADDSUB231PD : HB_INS_VFMADDSUB231PS;
        case 0xb7: return vex_w ? HB_INS_VFMSUBADD231PD : HB_INS_VFMSUBADD231PS;
        case 0xb8: return vex_w ? HB_INS_VFMADD231PD : HB_INS_VFMADD231PS;
        case 0xb9: return vex_w ? HB_INS_VFMADD231SD : HB_INS_VFMADD231SS;
        case 0xba: return vex_w ? HB_INS_VFMSUB231PD : HB_INS_VFMSUB231PS;
        case 0xbb: return vex_w ? HB_INS_VFMSUB231SD : HB_INS_VFMSUB231SS;
        case 0xbc: return vex_w ? HB_INS_VFNMADD231PD : HB_INS_VFNMADD231PS;
        case 0xbd: return vex_w ? HB_INS_VFNMADD231SD : HB_INS_VFNMADD231SS;
        case 0xbe: return vex_w ? HB_INS_VFNMSUB231PD : HB_INS_VFNMSUB231PS;
        case 0xbf: return vex_w ? HB_INS_VFNMSUB231SD : HB_INS_VFNMSUB231SS;
        default: return 0;
    }
}

static bool fma3_opcode_is_scalar(uint8_t op) {
    return op == 0x99 || op == 0x9b || op == 0x9d || op == 0x9f ||
           op == 0xa9 || op == 0xab || op == 0xad || op == 0xaf ||
           op == 0xb9 || op == 0xbb || op == 0xbd || op == 0xbf;
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

static hb_result_t parse_vsib_modrm(hb_dec_t* d, uint8_t modrm,
                                    bool rex_r, bool rex_x, bool rex_b,
                                    uint8_t mem_size, hb_decoded_t* out,
                                    int dst_slot, int mem_slot,
                                    uint8_t index_size, uint8_t elem_size,
                                    uint8_t elem_count) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg_op = (modrm >> 3) & 7;
    uint8_t rm = modrm & 7;
    if (mod == 3 || rm != 4) return HB_ERR_UNSUPPORTED_OPCODE;
    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    uint8_t sib = read_u8(d);
    out->has_modrm = true;
    out->mod = mod;
    out->reg_op = reg_op;
    out->rm = rm;
    out->has_sib = true;
    out->sib_scale = (sib >> 6) & 3;
    out->sib_index = (sib >> 3) & 7;
    out->sib_base = sib & 7;

    int base = -1;
    uint8_t scale = (uint8_t)(1u << out->sib_scale);
    int index = HB_REG_XMM0 + (int)(out->sib_index | (rex_x ? 8 : 0));
    int sb = reg_idx(out->sib_base, rex_b);
    if (out->sib_base == 5) {
        if (mod == 0) base = -1;
        else base = sb;
    } else {
        base = sb;
    }

    int64_t disp = 0;
    if (mod == 1) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        disp = read_s8(d);
    } else if (mod == 2 || (out->sib_base == 5 && mod == 0)) {
        if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
        disp = read_s32(d);
    }

    set_reg_ex(out, dst_slot, reg_idx(reg_op, rex_r), mem_size, 0);
    set_mem(out, mem_slot, base, index, scale, disp, mem_size);
    if (mem_slot == 1) {
        out->op1.mem.vsib = true;
        out->op1.mem.vsib_index_size = index_size;
        out->op1.mem.vsib_elem_size = elem_size;
        out->op1.mem.vsib_count = elem_count;
    } else if (mem_slot == 2) {
        out->op2.mem.vsib = true;
        out->op2.mem.vsib_index_size = index_size;
        out->op2.mem.vsib_elem_size = elem_size;
        out->op2.mem.vsib_count = elem_count;
    } else {
        out->op3.mem.vsib = true;
        out->op3.mem.vsib_index_size = index_size;
        out->op3.mem.vsib_elem_size = elem_size;
        out->op3.mem.vsib_count = elem_count;
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

static hb_result_t decode_x87_x64(hb_dec_t* d, uint8_t opcode, bool rex_b, hb_decoded_t* out) {
    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
    uint8_t modrm = read_u8(d);
    uint8_t mod = (modrm >> 6) & 3u;
    uint8_t reg_op = (modrm >> 3) & 7u;
    uint8_t size = 0;
    out->writes_flags = false;

    if (opcode == 0xdf && modrm == 0xe0) {
        out->opcode = HB_INS_X87_FNSTSW;
        set_reg(out, 1, HB_REG_RAX, 2);
        return HB_OK;
    }
    if (mod == 3) {
        uint8_t sti = modrm & 7u;
        if (opcode == 0xd8 || opcode == 0xdc) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FADD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FMUL;
            else if (modrm >= 0xd0 && modrm <= 0xd7) out->opcode = HB_INS_X87_FCOM;
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCOMP;
            else if (modrm >= 0xe0 && modrm <= 0xe7) out->opcode = opcode == 0xdc ? HB_INS_X87_FSUBR : HB_INS_X87_FSUB;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = opcode == 0xdc ? HB_INS_X87_FSUB : HB_INS_X87_FSUBR;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = opcode == 0xdc ? HB_INS_X87_FDIVR : HB_INS_X87_FDIV;
            else if (modrm >= 0xf8) out->opcode = opcode == 0xdc ? HB_INS_X87_FDIV : HB_INS_X87_FDIVR;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xd9) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FLD;
            else if (modrm >= 0xc8 && modrm <= 0xcf) out->opcode = HB_INS_X87_FXCH;
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FSTP;
            else if (modrm == 0xd0) { out->opcode = HB_INS_NOP; return HB_OK; }
            else if (modrm == 0xfc) { out->opcode = HB_INS_X87_FRNDINT; return HB_OK; }
            else if ((modrm >= 0xe0 && modrm <= 0xff) && modrm != 0xfc) {
                out->opcode = HB_INS_X87_MISC;
                return HB_OK;
            } else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xda) {
            if (modrm >= 0xc0 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCMOV;
            else if (modrm == 0xe9) out->opcode = HB_INS_X87_FCOMPP;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xdb) {
            if (modrm >= 0xc0 && modrm <= 0xdf) out->opcode = HB_INS_X87_FCMOV;
            else if (modrm == 0xe2) { out->opcode = HB_INS_X87_FNCLEX; return HB_OK; }
            else if (modrm == 0xe3) { out->opcode = HB_INS_X87_FNINIT; return HB_OK; }
            else if (modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe4) { out->opcode = HB_INS_NOP; return HB_OK; }
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FUCOMI;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FCOMI;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        if (opcode == 0xdd) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FFREE;
            else if (modrm >= 0xd0 && modrm <= 0xd7) out->opcode = HB_INS_X87_FST;
            else if (modrm >= 0xd8 && modrm <= 0xdf) out->opcode = HB_INS_X87_FSTP;
            else if (modrm >= 0xe0 && modrm <= 0xe7) out->opcode = HB_INS_X87_FUCOM;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FUCOMP;
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
        if (opcode == 0xdf) {
            if (modrm >= 0xc0 && modrm <= 0xc7) out->opcode = HB_INS_X87_FFREEP;
            else if (modrm >= 0xe8 && modrm <= 0xef) out->opcode = HB_INS_X87_FUCOMPI;
            else if (modrm >= 0xf0 && modrm <= 0xf7) out->opcode = HB_INS_X87_FCOMPI;
            else return HB_ERR_UNSUPPORTED_OPCODE;
            set_imm(out, 1, sti, 1);
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    switch (opcode) {
        case 0xd8:
        case 0xdc:
            size = (opcode == 0xd8) ? 4 : 8;
            if (reg_op == 0) out->opcode = HB_INS_X87_FADD;
            else if (reg_op == 1) out->opcode = HB_INS_X87_FMUL;
            else if (reg_op == 2) out->opcode = HB_INS_X87_FCOM;
            else if (reg_op == 3) out->opcode = HB_INS_X87_FCOMP;
            else if (reg_op == 4) out->opcode = HB_INS_X87_FSUB;
            else if (reg_op == 5) out->opcode = HB_INS_X87_FSUBR;
            else if (reg_op == 6) out->opcode = HB_INS_X87_FDIV;
            else out->opcode = HB_INS_X87_FDIVR;
            break;
        case 0xd9:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FLD; size = 4; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 4; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FSTP; size = 4; }
            else if (reg_op == 4 || reg_op == 6) { out->opcode = HB_INS_X87_MISC; size = 28; }
            else if (reg_op == 5) { out->opcode = HB_INS_NOP; size = 2; }
            else if (reg_op == 7) { out->opcode = HB_INS_MOV; size = 2; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xda:
            out->opcode = HB_INS_X87_FI;
            size = 4;
            break;
        case 0xdb:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 4; }
            else if (reg_op == 1) { out->opcode = HB_INS_X87_FISTTP; size = 4; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 4; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FISTP; size = 4; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FLD; size = 10; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FSTP; size = 10; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xde:
            out->opcode = HB_INS_X87_FI;
            size = 2;
            break;
        case 0xdd:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FLD; size = 8; }
            else if (reg_op == 1) { out->opcode = HB_INS_X87_FISTTP; size = 8; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 8; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FSTP; size = 8; }
            else if (reg_op == 4 || reg_op == 6) { out->opcode = HB_INS_X87_MISC; size = 108; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FNSTSW; size = 2; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        case 0xdf:
            if (reg_op == 0) { out->opcode = HB_INS_X87_FILD; size = 2; }
            else if (reg_op == 1) { out->opcode = HB_INS_X87_FISTTP; size = 2; }
            else if (reg_op == 2) { out->opcode = HB_INS_X87_FST; size = 2; }
            else if (reg_op == 3) { out->opcode = HB_INS_X87_FISTP; size = 2; }
            else if (reg_op == 4) { out->opcode = HB_INS_X87_MISC; size = 10; }
            else if (reg_op == 5) { out->opcode = HB_INS_X87_FILD; size = 8; }
            else if (reg_op == 6) { out->opcode = HB_INS_X87_MISC; size = 10; }
            else if (reg_op == 7) { out->opcode = HB_INS_X87_FISTP; size = 8; }
            else return HB_ERR_UNSUPPORTED_OPCODE;
            break;
        default:
            return HB_ERR_UNSUPPORTED_OPCODE;
    }
    hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, size, out, 1);
    if (r != HB_OK) return r;
    if (out->opcode == HB_INS_MOV) set_imm(out, 2, HB_X87_DEFAULT_CONTROL_WORD, 2);
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

    /* Prefix order is not guaranteed in fuzzed/probed byte streams.  Treat
       legacy prefixes and REX as one prefix run so capstone-valid mixed-order
       streams decode to the same first instruction. */
    while (can_read(d, 1)) {
        uint8_t b = d->code[d->pos];
        if (b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e ||
            b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3)
        {
            if (has_rex) {
                has_rex = false;
                rex_w = rex_r = rex_x = rex_b = false;
            }
            if (b == 0x66) operand16 = true;
            else if (b == 0x67) address32 = true;
            else if (b == 0xf2) prefix_f2 = true;
            else if (b == 0xf3) prefix_f3 = true;
            else if (b == 0x64 || b == 0x65) out->segment_prefix = b;
            d->pos++;
        }
        else if (b >= 0x40 && b <= 0x4F) {
            has_rex = true;
            rex_w = (b >> 3) & 1;
            rex_r = (b >> 2) & 1;
            rex_x = (b >> 1) & 1;
            rex_b = b & 1;
            d->pos++;
        }
        else break;
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

    if (opcode == 0xC5 || opcode == 0xC4 || opcode == 0x62) {
        uint8_t vex_opcode, vex_map = 1, vex_pp = 0, vex_l = 0, vex_w = 0, vex_v = 0;
        uint8_t evex_p0 = 0, evex_p2 = 0, evex_aaa = 0, evex_ll = 0;
        bool evex_z = false, evex_r2 = false, evex_b = false;
        if (opcode == 0xC5) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            uint8_t vex2 = read_u8(d);
            rex_r = ((~vex2 >> 7) & 1) != 0;
            vex_v = (uint8_t)((~vex2 >> 3) & 0x0f);
            vex_l = (vex2 >> 2) & 1;
            vex_pp = vex2 & 3;
            vex_opcode = read_u8(d);
        } else if (opcode == 0xC4) {
            if (!can_read(d, 3)) return HB_ERR_DECODE_FAILED;
            uint8_t vex2 = read_u8(d);
            uint8_t vex3 = read_u8(d);
            rex_r = ((~vex2 >> 7) & 1) != 0;
            rex_x = ((~vex2 >> 6) & 1) != 0;
            rex_b = ((~vex2 >> 5) & 1) != 0;
            vex_map = vex2 & 0x1f;
            vex_w = (vex3 >> 7) & 1;
            vex_v = (uint8_t)((~vex3 >> 3) & 0x0f);
            vex_l = (vex3 >> 2) & 1;
            vex_pp = vex3 & 3;
            vex_opcode = read_u8(d);
        } else {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            evex_p0 = read_u8(d);
            uint8_t evex_p1 = read_u8(d);
            evex_p2 = read_u8(d);
            rex_r = ((~evex_p0 >> 7) & 1) != 0;
            rex_x = ((~evex_p0 >> 6) & 1) != 0;
            rex_b = ((~evex_p0 >> 5) & 1) != 0;
            evex_r2 = ((~evex_p0 >> 4) & 1) != 0;
            vex_map = evex_p0 & 0x03;
            vex_w = (evex_p1 >> 7) & 1;
            vex_v = (uint8_t)((~evex_p1 >> 3) & 0x0f);
            if (((evex_p2 >> 3) & 1) == 0) vex_v |= 0x10;
            vex_pp = evex_p1 & 3;
            evex_z = (evex_p2 >> 7) != 0;
            evex_ll = (evex_p2 >> 5) & 3;
            evex_b = ((evex_p2 >> 4) & 1) != 0;
            evex_aaa = evex_p2 & 7;
            vex_opcode = read_u8(d);
            out->evex = true;
            out->evex_mask = evex_aaa;
            out->evex_zero = evex_z;
            out->evex_broadcast = false;
            out->evex_rounding = 0;
            out->evex_mask_lane = 0;
        }
        out->writes_flags = false;
        if (opcode == 0x62) {
            uint8_t vec_size = evex_ll == 2 ? 64 : (evex_ll == 1 ? 32 : 16);
            int mapped = 0;
            if (vex_opcode == 0x77) {
                out->opcode = HB_INS_VEC;
                return HB_OK;
            }
            if (vex_map == 1) {
                if ((vex_opcode == 0x10 || vex_opcode == 0x11 ||
                     vex_opcode == 0x28 || vex_opcode == 0x29) &&
                    vex_pp == 0 && !vex_w) { mapped = HB_INS_SSE_MOV; out->evex_mask_lane = 4; }
                else if ((vex_opcode == 0x10 || vex_opcode == 0x11 ||
                          vex_opcode == 0x28 || vex_opcode == 0x29) &&
                         vex_pp == 1 && vex_w) { mapped = HB_INS_SSE_MOV; out->evex_mask_lane = 8; }
                else if ((vex_opcode == 0x10 || vex_opcode == 0x11) &&
                         vex_pp == 2 && !vex_w) { mapped = HB_INS_SSE_MOV; out->evex_mask_lane = 4; }
                else if ((vex_opcode == 0x10 || vex_opcode == 0x11) &&
                         vex_pp == 3 && vex_w) { mapped = HB_INS_SSE_MOV; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x14 && vex_pp == 0 && !vex_w) { mapped = HB_INS_UNPCKLPS; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x14 && vex_pp == 1 && vex_w) { mapped = HB_INS_UNPCKLPD; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x15 && vex_pp == 0 && !vex_w) { mapped = HB_INS_UNPCKHPS; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x15 && vex_pp == 1 && vex_w) { mapped = HB_INS_UNPCKHPD; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x70 && (vex_pp == 1 || vex_pp == 2 || vex_pp == 3) && !vex_w) {
                    uint8_t mask_lane = vex_pp == 1 ? 4 : 2;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_pp == 1 ? HB_INS_PSHUFD :
                                  (vex_pp == 2 ? HB_INS_PSHUFHW : HB_INS_PSHUFLW);
                    out->evex_mask_lane = mask_lane;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    int evex_reg = (int)(((modrm >> 3) & 7u) | (rex_r ? 8u : 0u) | (evex_r2 ? 16u : 0u));
                    int evex_rm_reg = (int)((modrm & 7u) | (rex_b ? 8u : 0u) | (rex_x ? 16u : 0u));
                    replace_reg_index(out, 1, evex_reg);
                    if (((modrm >> 6) & 3u) == 3) replace_reg_index(out, 2, evex_rm_reg);
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                    else if (out->op2.is_mem) out->op2.size = vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_imm(out, 3, read_u8(d), 1);
                    return HB_OK;
                }
                else if (vex_opcode == 0xc6 && vex_pp == 0 && !vex_w) { mapped = HB_INS_SHUFPS; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0xc6 && vex_pp == 1 && vex_w) { mapped = HB_INS_SHUFPD; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x58 && vex_pp == 0 && !vex_w) mapped = HB_INS_ADDPS;
                else if (vex_opcode == 0x58 && vex_pp == 1 && vex_w) mapped = HB_INS_ADDPD;
                else if (vex_opcode == 0x58 && vex_pp == 2 && !vex_w) mapped = HB_INS_ADDSS;
                else if (vex_opcode == 0x58 && vex_pp == 3 && vex_w) mapped = HB_INS_ADDSD;
                else if (vex_opcode == 0x51 && vex_pp == 0 && !vex_w) mapped = HB_INS_SQRTPS;
                else if (vex_opcode == 0x51 && vex_pp == 1 && vex_w) mapped = HB_INS_SQRTPD;
                else if (vex_opcode == 0x51 && vex_pp == 2 && !vex_w) mapped = HB_INS_SQRTSS;
                else if (vex_opcode == 0x51 && vex_pp == 3 && vex_w) mapped = HB_INS_SQRTSD;
                else if (vex_opcode == 0x54 && vex_pp == 0 && !vex_w) { mapped = HB_INS_XMM_AND; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x54 && vex_pp == 1 && vex_w) { mapped = HB_INS_XMM_AND; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x55 && vex_pp == 0 && !vex_w) { mapped = HB_INS_XMM_ANDN; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x55 && vex_pp == 1 && vex_w) { mapped = HB_INS_XMM_ANDN; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x56 && vex_pp == 0 && !vex_w) { mapped = HB_INS_XMM_OR; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x56 && vex_pp == 1 && vex_w) { mapped = HB_INS_XMM_OR; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x57 && vex_pp == 0 && !vex_w) { mapped = HB_INS_XORPS; out->evex_mask_lane = 4; }
                else if (vex_opcode == 0x57 && vex_pp == 1 && vex_w) { mapped = HB_INS_XORPS; out->evex_mask_lane = 8; }
                else if (vex_opcode == 0x59 && vex_pp == 0 && !vex_w) mapped = HB_INS_MULPS;
                else if (vex_opcode == 0x59 && vex_pp == 1 && vex_w) mapped = HB_INS_MULPD;
                else if (vex_opcode == 0x59 && vex_pp == 2 && !vex_w) mapped = HB_INS_MULSS;
                else if (vex_opcode == 0x59 && vex_pp == 3 && vex_w) mapped = HB_INS_MULSD;
                else if (vex_opcode == 0x5c && vex_pp == 0 && !vex_w) mapped = HB_INS_SUBPS;
                else if (vex_opcode == 0x5c && vex_pp == 1 && vex_w) mapped = HB_INS_SUBPD;
                else if (vex_opcode == 0x5c && vex_pp == 2 && !vex_w) mapped = HB_INS_SUBSS;
                else if (vex_opcode == 0x5c && vex_pp == 3 && vex_w) mapped = HB_INS_SUBSD;
                else if (vex_opcode == 0x5d && vex_pp == 0 && !vex_w) mapped = HB_INS_MINPS;
                else if (vex_opcode == 0x5d && vex_pp == 1 && vex_w) mapped = HB_INS_MINPD;
                else if (vex_opcode == 0x5d && vex_pp == 2 && !vex_w) mapped = HB_INS_MINSS;
                else if (vex_opcode == 0x5d && vex_pp == 3 && vex_w) mapped = HB_INS_MINSD;
                else if (vex_opcode == 0x5e && vex_pp == 0 && !vex_w) mapped = HB_INS_DIVPS;
                else if (vex_opcode == 0x5e && vex_pp == 1 && vex_w) mapped = HB_INS_DIVPD;
                else if (vex_opcode == 0x5e && vex_pp == 2 && !vex_w) mapped = HB_INS_DIVSS;
                else if (vex_opcode == 0x5e && vex_pp == 3 && vex_w) mapped = HB_INS_DIVSD;
                else if (vex_opcode == 0x5f && vex_pp == 0 && !vex_w) mapped = HB_INS_MAXPS;
                else if (vex_opcode == 0x5f && vex_pp == 1 && vex_w) mapped = HB_INS_MAXPD;
                else if (vex_opcode == 0x5f && vex_pp == 2 && !vex_w) mapped = HB_INS_MAXSS;
                else if (vex_opcode == 0x5f && vex_pp == 3 && vex_w) mapped = HB_INS_MAXSD;
                else if (vex_opcode == 0xdb && vex_pp == 1) { mapped = HB_INS_XMM_AND; out->evex_mask_lane = vex_w ? 8 : 4; }
                else if (vex_opcode == 0xdf && vex_pp == 1) { mapped = HB_INS_XMM_ANDN; out->evex_mask_lane = vex_w ? 8 : 4; }
                else if (vex_opcode == 0xeb && vex_pp == 1) { mapped = HB_INS_XMM_OR; out->evex_mask_lane = vex_w ? 8 : 4; }
                else if (vex_opcode == 0xef && vex_pp == 1) { mapped = HB_INS_PXOR; out->evex_mask_lane = vex_w ? 8 : 4; }
                else if ((vex_opcode == 0x6f || vex_opcode == 0x7f) && vex_pp == 1) {
                    mapped = HB_INS_SSE_MOV;
                    out->evex_mask_lane = vex_w ? 8 : 4;
                }
                else if ((vex_opcode == 0x6f || vex_opcode == 0x7f) && vex_pp == 2) {
                    mapped = HB_INS_SSE_MOV;
                    out->evex_mask_lane = vex_w ? 8 : 4;
                }
                else if ((vex_opcode == 0x6f || vex_opcode == 0x7f) && vex_pp == 3) {
                    mapped = HB_INS_SSE_MOV;
                    out->evex_mask_lane = vex_w ? 2 : 1;
                }
                else if (vex_pp == 1) {
                    if (vex_w) {
                        switch (vex_opcode) {
                            case 0x6c: mapped = HB_INS_PUNPCKLQDQ; break;
                            case 0x6d: mapped = HB_INS_PUNPCKHQDQ; break;
                            case 0xd4: mapped = HB_INS_PADDQ; break;
                            case 0xf4: mapped = HB_INS_PMULUDQ; break;
                            case 0xf6: mapped = HB_INS_PSADBW; break;
                            case 0xfb: mapped = HB_INS_PSUBQ; break;
                            default: break;
                        }
                    } else {
                        switch (vex_opcode) {
                            case 0xfc: mapped = HB_INS_PADDB; break;
                            case 0xfd: mapped = HB_INS_PADDW; break;
                            case 0xfe: mapped = HB_INS_PADDD; break;
                            case 0xf8: mapped = HB_INS_PSUBB; break;
                            case 0xf9: mapped = HB_INS_PSUBW; break;
                            case 0xfa: mapped = HB_INS_PSUBD; break;
                            default: break;
                        }
                        if (!mapped) mapped = sse2_0f_unpack_pack_opcode(vex_opcode);
                        if (mapped == HB_INS_PUNPCKLQDQ || mapped == HB_INS_PUNPCKHQDQ) mapped = 0;
                        if (!mapped && !sse2_0f_variable_shift_opcode(vex_opcode) &&
                            vex_opcode != 0xc4 && vex_opcode != 0xc5) {
                            mapped = sse2_0f_packed_opcode(vex_opcode);
                            if (mapped == HB_INS_PADDQ || mapped == HB_INS_PSUBQ ||
                                mapped == HB_INS_PMULUDQ) {
                                mapped = 0;
                            }
                        }
                    }
                    if (mapped) out->evex_mask_lane = evex_packed_int_mask_lane(mapped);
                    if (mapped && out->evex_mask_lane == 0) mapped = 0;
                }
            } else if (vex_map == 2 && vex_pp == 1) {
                if (vex_opcode == 0x18 || vex_opcode == 0x19 ||
                    vex_opcode == 0x1a || vex_opcode == 0x1b) {
                    uint8_t src_bytes = 0;
                    uint8_t mask_lane = 0;
                    bool allow_reg_src = false;
                    if (vex_opcode == 0x18 && !vex_w) {
                        mapped = HB_INS_VBROADCASTSS;
                        src_bytes = 4;
                        mask_lane = 4;
                        allow_reg_src = true;
                    } else if (vex_opcode == 0x19 && !vex_w) {
                        mapped = HB_INS_VBROADCASTF32X2;
                        src_bytes = 8;
                        mask_lane = 4;
                        allow_reg_src = true;
                    } else if (vex_opcode == 0x19 && vex_w) {
                        mapped = HB_INS_VBROADCASTSD;
                        src_bytes = 8;
                        mask_lane = 8;
                        allow_reg_src = true;
                    } else if (vex_opcode == 0x1a && !vex_w) {
                        mapped = HB_INS_VBROADCASTF32X4;
                        src_bytes = 16;
                        mask_lane = 4;
                    } else if (vex_opcode == 0x1a && vex_w) {
                        mapped = HB_INS_VBROADCASTF64X2;
                        src_bytes = 16;
                        mask_lane = 8;
                    } else if (vex_opcode == 0x1b && !vex_w) {
                        if (evex_ll != 2) return HB_ERR_UNSUPPORTED_OPCODE;
                        mapped = HB_INS_VBROADCASTF32X8;
                        src_bytes = 32;
                        mask_lane = 4;
                    } else if (vex_opcode == 0x1b && vex_w) {
                        if (evex_ll != 2) return HB_ERR_UNSUPPORTED_OPCODE;
                        mapped = HB_INS_VBROADCASTF64X4;
                        src_bytes = 32;
                        mask_lane = 8;
                    } else {
                        return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if (((modrm >> 6) & 3u) == 3 && !allow_reg_src) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = mapped;
                    out->evex_mask_lane = mask_lane;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                src_bytes, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = src_bytes;
                    return HB_OK;
                }
                if (vex_opcode == 0x78 || vex_opcode == 0x79 ||
                    vex_opcode == 0x58 || vex_opcode == 0x59) {
                    uint8_t lane = 0;
                    uint8_t src_bytes = 0;
                    if (vex_opcode == 0x78) { mapped = HB_INS_VPBROADCASTB; lane = 1; }
                    else if (vex_opcode == 0x79) { mapped = HB_INS_VPBROADCASTW; lane = 2; }
                    else if (vex_opcode == 0x58 && !vex_w) { mapped = HB_INS_VPBROADCASTD; lane = 4; }
                    else if (vex_opcode == 0x59 && !vex_w) { mapped = HB_INS_VBROADCASTI32X2; lane = 4; src_bytes = 8; }
                    else if (vex_opcode == 0x59 && vex_w) { mapped = HB_INS_VPBROADCASTQ; lane = 8; }
                    else return HB_ERR_UNSUPPORTED_OPCODE;
                    if (!src_bytes) src_bytes = lane;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = mapped;
                    out->evex_mask_lane = lane;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                src_bytes, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = src_bytes;
                    return HB_OK;
                }
                if (vex_opcode == 0xcf) mapped = HB_INS_VGF2P8MULB;
                else if (vex_opcode >= 0xdc && vex_opcode <= 0xdf) {
                    switch (vex_opcode) {
                        case 0xdc: mapped = HB_INS_VAESENC; break;
                        case 0xdd: mapped = HB_INS_VAESENCLAST; break;
                        case 0xde: mapped = HB_INS_VAESDEC; break;
                        case 0xdf: mapped = HB_INS_VAESDECLAST; break;
                        default: break;
                    }
                }
                else if (!vex_w) {
                    mapped = ssse3_0f38_opcode(vex_opcode);
                    if (mapped) out->evex_mask_lane = evex_packed_int_mask_lane(mapped);
                    if (mapped && out->evex_mask_lane == 0) mapped = 0;
                }
            }
            if (mapped) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                bool evex_broadcast = false;
                uint8_t evex_broadcast_size = 0;
                if (evex_b) {
                    if (mapped == HB_INS_ADDPS || mapped == HB_INS_SUBPS ||
                        mapped == HB_INS_MULPS || mapped == HB_INS_DIVPS ||
                        mapped == HB_INS_ADDPD || mapped == HB_INS_SUBPD ||
                        mapped == HB_INS_MULPD || mapped == HB_INS_DIVPD ||
                        mapped == HB_INS_MINPS || mapped == HB_INS_MAXPS ||
                        mapped == HB_INS_MINPD || mapped == HB_INS_MAXPD ||
                        mapped == HB_INS_XMM_AND || mapped == HB_INS_XMM_ANDN ||
                        mapped == HB_INS_XMM_OR || mapped == HB_INS_XORPS ||
                        mapped == HB_INS_PXOR) {
                        if ((modrm >> 6) == 3) {
                            if (mapped == HB_INS_ADDPS) out->evex_rounding = (uint8_t)(evex_ll + 1);
                            else return HB_ERR_UNSUPPORTED_OPCODE;
                        } else {
                            if (evex_ll != 2) return HB_ERR_UNSUPPORTED_OPCODE;
                            evex_broadcast = true;
                            evex_broadcast_size = (mapped == HB_INS_ADDPD || mapped == HB_INS_SUBPD ||
                                                   mapped == HB_INS_MULPD || mapped == HB_INS_DIVPD ||
                                                   mapped == HB_INS_MINPD || mapped == HB_INS_MAXPD ||
                                                   out->evex_mask_lane == 8) ? 8 : 4;
                        }
                        vec_size = 64;
                    } else {
                        return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                }
                out->opcode = mapped;
                out->evex_broadcast = evex_broadcast;
                bool is_mov = mapped == HB_INS_SSE_MOV;
                bool scalar_evex_mov = is_mov && (vex_opcode == 0x10 || vex_opcode == 0x11) &&
                                       (vex_pp == 2 || vex_pp == 3);
                bool scalar_evex = mapped == HB_INS_ADDSS || mapped == HB_INS_ADDSD ||
                                   mapped == HB_INS_SUBSS || mapped == HB_INS_SUBSD ||
                                   mapped == HB_INS_MULSS || mapped == HB_INS_MULSD ||
                                   mapped == HB_INS_DIVSS || mapped == HB_INS_DIVSD ||
                                   mapped == HB_INS_SQRTSS || mapped == HB_INS_SQRTSD ||
                                   mapped == HB_INS_MINSS || mapped == HB_INS_MINSD ||
                                   mapped == HB_INS_MAXSS || mapped == HB_INS_MAXSD;
                uint8_t operand_vec_size = (scalar_evex || scalar_evex_mov) ? 16 : vec_size;
                uint8_t scalar_lane = (mapped == HB_INS_ADDSD || mapped == HB_INS_SUBSD ||
                                       mapped == HB_INS_MULSD || mapped == HB_INS_DIVSD ||
                                       mapped == HB_INS_SQRTSD || mapped == HB_INS_MINSD ||
                                       mapped == HB_INS_MAXSD || (scalar_evex_mov && vex_pp == 3)) ? 8 : 4;
                bool store = is_mov && (vex_opcode == 0x11 || vex_opcode == 0x29 || vex_opcode == 0x7f);
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                            operand_vec_size, out, 1, 2, store);
                if (r != HB_OK) return r;
                int evex_reg = (int)(((modrm >> 3) & 7u) | (rex_r ? 8u : 0u) | (evex_r2 ? 16u : 0u));
                int evex_rm_reg = (int)((modrm & 7u) | (rex_b ? 8u : 0u) | (rex_x ? 16u : 0u));
                if (store) {
                    replace_reg_index(out, 1, evex_rm_reg);
                    replace_reg_index(out, 2, evex_reg);
                } else {
                    replace_reg_index(out, 1, evex_reg);
                    if (((modrm >> 6) & 3u) == 3) replace_reg_index(out, 2, evex_rm_reg);
                }
                if (scalar_evex_mov) {
                    if (((modrm >> 6) & 3u) == 3) {
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_vec_operand(out, 1, 16);
                        mark_vec_operand(out, 2, 16);
                        mark_vec_operand(out, 3, scalar_lane);
                    } else if (store) {
                        if (out->op1.is_mem) out->op1.size = scalar_lane;
                        mark_vec_operand(out, 2, 16);
                    } else {
                        mark_vec_operand(out, 1, 16);
                        if (out->op2.is_mem) out->op2.size = scalar_lane;
                    }
                    return HB_OK;
                }
                if (is_mov) {
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                hb_decoded_t tmp = *out;
                out->op3 = tmp.op2;
                memset(&out->op2, 0, sizeof(out->op2));
                set_reg(out, 2, vex_v, operand_vec_size);
                mark_vec_operand(out, 1, operand_vec_size);
                mark_vec_operand(out, 2, operand_vec_size);
                mark_vec_operand(out, 3, operand_vec_size);
                if (out->op3.is_mem) out->op3.size = scalar_evex ? scalar_lane : (evex_broadcast ? evex_broadcast_size : vec_size);
                if (mapped == HB_INS_SHUFPS || mapped == HB_INS_SHUFPD) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                }
                return HB_OK;
            }
            if (vex_map == 1 && vex_opcode == 0xc2) {
                bool scalar = vex_pp == 2 || vex_pp == 3;
                bool is_pd = vex_pp == 1 || vex_pp == 3;
                uint8_t lane = is_pd ? 8 : 4;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                switch (vex_pp) {
                    case 0: out->opcode = HB_INS_VCMPPS; break;
                    case 1: out->opcode = HB_INS_VCMPPD; break;
                    case 2: out->opcode = HB_INS_VCMPSS; break;
                    case 3: out->opcode = HB_INS_VCMPSD; break;
                    default: return HB_ERR_UNSUPPORTED_OPCODE;
                }
                bool evex_broadcast = evex_b && ((modrm >> 6) != 3) && !scalar;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                            scalar ? 16 : vec_size, out, 1, 3, false);
                if (r != HB_OK) return r;
                int evex_rm_reg = (int)((modrm & 7u) | (rex_b ? 8u : 0u) | (rex_x ? 16u : 0u));
                uint8_t kdst = (modrm >> 3) & 7u;
                memset(&out->op1, 0, sizeof(out->op1));
                set_imm(out, 1, kdst, 1);
                memset(&out->op2, 0, sizeof(out->op2));
                set_reg(out, 2, vex_v, scalar ? 16 : vec_size);
                if (((modrm >> 6) & 3u) == 3) replace_reg_index(out, 3, evex_rm_reg);
                mark_vec_operand(out, 2, scalar ? 16 : vec_size);
                if (out->op3.is_reg) mark_vec_operand(out, 3, scalar ? 16 : vec_size);
                else if (out->op3.is_mem) out->op3.size = evex_broadcast ? lane : (scalar ? lane : vec_size);
                out->evex_broadcast = evex_broadcast;
                out->evex_mask_lane = lane;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                return HB_OK;
            }
            out->opcode = HB_INS_VEC;
        } else if (vex_opcode == 0x77 && vex_map == 1) {
            out->opcode = HB_INS_VZEROUPPER;
            return HB_OK;
        } else {
            uint8_t vec_size = vex_l ? 32 : 16;
            int mapped = 0;
            if (vex_map == 2 && !vex_l &&
                ((vex_pp == 0 && (vex_opcode == 0xf2 || vex_opcode == 0xf3 ||
                                  vex_opcode == 0xf5 || vex_opcode == 0xf7)) ||
                 ((vex_pp == 1 || vex_pp == 2 || vex_pp == 3) && vex_opcode == 0xf7) ||
                 ((vex_pp == 2 || vex_pp == 3) && vex_opcode == 0xf5) ||
                 (vex_pp == 3 && vex_opcode == 0xf6))) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                uint8_t ext = (modrm >> 3) & 7;
                uint8_t size = vex_w ? 8 : 4;
                if (vex_pp == 0 && vex_opcode == 0xf2) {
                    out->opcode = HB_INS_ANDN;
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 3, false);
                    if (r != HB_OK) return r;
                    set_reg(out, 2, vex_v, size);
                    out->op1.size = size;
                    out->op2.size = size;
                    out->op3.size = size;
                    return HB_OK;
                }
                if (vex_pp == 0 && vex_opcode == 0xf3) {
                    if (ext == 1) out->opcode = HB_INS_BLSR;
                    else if (ext == 2) out->opcode = HB_INS_BLSMSK;
                    else if (ext == 3) out->opcode = HB_INS_BLSI;
                    else return HB_ERR_UNSUPPORTED_OPCODE;
                    hb_result_t r = parse_modrm_ext(d, modrm, vex_w, rex_b, 4, out, 2);
                    if (r != HB_OK) return r;
                    set_reg(out, 1, vex_v, size);
                    out->op2.size = size;
                    return HB_OK;
                }
                if ((vex_pp == 0 && (vex_opcode == 0xf5 || vex_opcode == 0xf7)) ||
                    ((vex_pp == 1 || vex_pp == 2 || vex_pp == 3) && vex_opcode == 0xf7) ||
                    ((vex_pp == 2 || vex_pp == 3) && vex_opcode == 0xf5)) {
                    if (vex_pp == 0 && vex_opcode == 0xf5) out->opcode = HB_INS_BZHI;
                    else if (vex_pp == 0 && vex_opcode == 0xf7) out->opcode = HB_INS_BEXTR;
                    else if (vex_pp == 1 && vex_opcode == 0xf7) out->opcode = HB_INS_SHLX;
                    else if (vex_pp == 2 && vex_opcode == 0xf7) out->opcode = HB_INS_SARX;
                    else if (vex_pp == 3 && vex_opcode == 0xf7) out->opcode = HB_INS_SHRX;
                    else if (vex_pp == 2 && vex_opcode == 0xf5) out->opcode = HB_INS_PEXT;
                    else if (vex_pp == 3 && vex_opcode == 0xf5) out->opcode = HB_INS_PDEP;
                    else return HB_ERR_UNSUPPORTED_OPCODE;
                    if (out->opcode == HB_INS_PEXT || out->opcode == HB_INS_PDEP) {
                        hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 3, false);
                        if (r != HB_OK) return r;
                        set_reg(out, 2, vex_v, size);
                        out->op1.size = size;
                        out->op2.size = size;
                        out->op3.size = size;
                        return HB_OK;
                    }
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    set_reg(out, 3, vex_v, size);
                    out->op1.size = size;
                    out->op2.size = size;
                    out->op3.size = size;
                    return HB_OK;
                }
                if (vex_pp == 3 && vex_opcode == 0xf6) {
                    out->opcode = HB_INS_MULX;
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 3, false);
                    if (r != HB_OK) return r;
                    set_reg(out, 2, vex_v, size);
                    out->op1.size = size;
                    out->op2.size = size;
                    out->op3.size = size;
                    return HB_OK;
                }
            }
            if (vex_map == 3 && vex_pp == 3 && !vex_l && vex_opcode == 0xf0) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                uint8_t size = vex_w ? 8 : 4;
                out->opcode = HB_INS_RORX;
                hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
                out->op1.size = size;
                out->op2.size = size;
                return HB_OK;
            }
            if (vex_map == 1) {
                if (vex_opcode == 0xc2) {
                    uint8_t lane = (vex_pp == 1 || vex_pp == 3) ? 8 : 4;
                    bool scalar = vex_pp == 2 || vex_pp == 3;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    switch (vex_pp) {
                        case 0: out->opcode = HB_INS_VCMPPS; break;
                        case 1: out->opcode = HB_INS_VCMPPD; break;
                        case 2: out->opcode = HB_INS_VCMPSS; break;
                        case 3: out->opcode = HB_INS_VCMPSD; break;
                        default: return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                scalar ? 16 : vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, scalar ? 16 : vec_size);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, scalar ? 16 : vec_size);
                    else if (out->op3.is_mem) out->op3.size = scalar ? lane : vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                    return HB_OK;
                }
                if (vex_opcode == 0x6e && vex_pp == 1) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_MOVD;
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_xmm_operand(out, 1);
                    out->op2.size = vex_w ? 8 : 4;
                    return HB_OK;
                }
                if ((vex_opcode == 0x7e || vex_opcode == 0xd6) && vex_pp == 1) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t size = (vex_opcode == 0xd6 || vex_w) ? 8 : 4;
                    out->opcode = HB_INS_MOVD;
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b,
                                                size, out, 1, 2, true);
                    if (r != HB_OK) return r;
                    if (vex_opcode == 0xd6 && out->op1.is_reg) mark_xmm_operand(out, 1);
                    mark_xmm_operand(out, 2);
                    out->op1.size = size;
                    return HB_OK;
                }
                if (vex_opcode == 0x7e && vex_pp == 2) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_MOVD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                8, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_xmm_operand(out, 1);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    else if (out->op2.is_mem) out->op2.size = 8;
                    return HB_OK;
                }
                if ((vex_opcode == 0x50 && (vex_pp == 0 || vex_pp == 1)) ||
                    (vex_opcode == 0xd7 && vex_pp == 1)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0xd7 ? HB_INS_PMOVMSKB :
                                  (vex_pp == 1 ? HB_INS_MOVMSKPD : HB_INS_MOVMSKPS);
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!out->op2.is_reg) return HB_ERR_UNSUPPORTED_OPCODE;
                    mark_xmm_operand(out, 2);
                    out->op1.size = 4;
                    return HB_OK;
                }
                if (vex_opcode == 0xae && vex_pp == 0) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t ext = (modrm >> 3) & 7;
                    if (mod == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    if (ext == 2) {
                        out->opcode = HB_INS_NOP;
                        return parse_modrm_ext(d, modrm, false, rex_b, 4, out, 1);
                    }
                    if (ext == 3) {
                        out->opcode = HB_INS_MOV;
                        hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 4, out, 1);
                        if (r != HB_OK) return r;
                        set_imm(out, 2, HB_X64_DEFAULT_MXCSR, 4);
                        return HB_OK;
                    }
                    return HB_ERR_UNSUPPORTED_OPCODE;
                }
                if (vex_opcode == 0xf7 && vex_pp == 1 && !vex_l) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if ((modrm >> 6) != 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = HB_INS_VMASKMOVDQU;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                16, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_xmm_operand(out, 1);
                    mark_xmm_operand(out, 2);
                    return HB_OK;
                }
                if ((vex_opcode == 0xc4 || vex_opcode == 0xc5) && vex_pp == 1 && !vex_w && !vex_l) {
                    bool extract = vex_opcode == 0xc5;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = extract ? HB_INS_PEXTRW : HB_INS_PINSRW;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    if (extract) {
                        mark_xmm_operand(out, 2);
                    } else {
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_xmm_operand(out, 1);
                        mark_xmm_operand(out, 2);
                        if (out->op3.is_mem) out->op3.size = 2;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0x6f || vex_opcode == 0x7f) &&
                    (vex_pp == 1 || vex_pp == 2)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_SSE_MOV;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, vex_opcode == 0x7f);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                if ((vex_opcode == 0x10 || vex_opcode == 0x11 ||
                     vex_opcode == 0x28 || vex_opcode == 0x29) &&
                    (vex_pp == 0 || vex_pp == 1)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_SSE_MOV;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2,
                                                vex_opcode == 0x11 || vex_opcode == 0x29);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                if (((vex_opcode == 0x2b) && (vex_pp == 0 || vex_pp == 1)) ||
                    ((vex_opcode == 0xe7) && vex_pp == 1)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = HB_INS_SSE_MOV;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, true);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                if (vex_opcode == 0xf0 && vex_pp == 3) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = HB_INS_SSE_MOV;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                if ((vex_opcode == 0x2e || vex_opcode == 0x2f) &&
                    (vex_pp == 0 || vex_pp == 1)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    bool is_pd = vex_pp == 1;
                    out->opcode = is_pd ? HB_INS_COMISD : HB_INS_COMISS;
                    out->writes_flags = true;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                is_pd ? 8 : 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_xmm_operand(out, 1);
                    mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = is_pd ? 8 : 4;
                    return HB_OK;
                }
                if (vex_opcode == 0x70 && (vex_pp == 1 || vex_pp == 2 || vex_pp == 3)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if (vex_pp == 1) out->opcode = HB_INS_PSHUFD;
                    else if (vex_pp == 2) out->opcode = HB_INS_PSHUFHW;
                    else out->opcode = HB_INS_PSHUFLW;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                    else if (out->op2.is_mem) out->op2.size = vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_imm(out, 3, read_u8(d), 1);
                    return HB_OK;
                }
                if ((vex_opcode == 0x12 && (vex_pp == 2 || vex_pp == 3)) ||
                    (vex_opcode == 0x16 && vex_pp == 2)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if (vex_opcode == 0x12 && vex_pp == 2) out->opcode = HB_INS_VMOVSLDUP;
                    else if (vex_opcode == 0x12) out->opcode = HB_INS_VMOVDDUP;
                    else out->opcode = HB_INS_VMOVSHDUP;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                    else if (out->op2.is_mem) out->op2.size = vec_size;
                    return HB_OK;
                }
                if ((vex_opcode == 0x5a && (vex_pp == 0 || vex_pp == 1)) ||
                    (vex_opcode == 0x5b && (vex_pp == 0 || vex_pp == 1 || vex_pp == 2)) ||
                    (vex_opcode == 0xe6 && (vex_pp == 1 || vex_pp == 2 || vex_pp == 3))) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t dst_size = vec_size;
                    uint8_t src_size = vec_size;
                    if (vex_opcode == 0x5a) {
                        if (vex_pp == 0) {
                            out->opcode = HB_INS_CVTPS2PD;
                            src_size = vex_l ? 16 : 8;
                        } else {
                            out->opcode = HB_INS_CVTPD2PS;
                            dst_size = 16;
                            src_size = vex_l ? 32 : 16;
                        }
                    } else if (vex_opcode == 0x5b) {
                        if (vex_pp == 0) out->opcode = HB_INS_CVTDQ2PS;
                        else if (vex_pp == 1) out->opcode = HB_INS_CVTPS2DQ;
                        else out->opcode = HB_INS_CVTTPS2DQ;
                    } else {
                        if (vex_pp == 1) {
                            out->opcode = HB_INS_CVTTPD2DQ;
                            dst_size = 16;
                            src_size = vex_l ? 32 : 16;
                        } else if (vex_pp == 2) {
                            out->opcode = HB_INS_CVTDQ2PD;
                            src_size = vex_l ? 16 : 8;
                        } else {
                            out->opcode = HB_INS_CVTPD2DQ;
                            dst_size = 16;
                            src_size = vex_l ? 32 : 16;
                        }
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                src_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, dst_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, src_size);
                    else if (out->op2.is_mem) out->op2.size = src_size;
                    return HB_OK;
                }
                if (vex_opcode == 0x5a && (vex_pp == 2 || vex_pp == 3)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t lane = vex_pp == 3 ? 8 : 4;
                    out->opcode = vex_pp == 3 ? HB_INS_CVTSD2SS : HB_INS_CVTSS2SD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                lane, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, 16);
                    else if (out->op3.is_mem) out->op3.size = lane;
                    return HB_OK;
                }
                if (vex_opcode == 0x2a && (vex_pp == 2 || vex_pp == 3)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_pp == 3 ? HB_INS_CVTSI2SD : HB_INS_CVTSI2SS;
                    hb_result_t r = parse_modrm(d, modrm, vex_w, rex_r, rex_x, rex_b,
                                                vex_w ? 8 : 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    out->op3.size = vex_w ? 8 : 4;
                    return HB_OK;
                }
                if ((vex_opcode == 0x2c || vex_opcode == 0x2d) &&
                    (vex_pp == 2 || vex_pp == 3)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    bool is_sd = vex_pp == 3;
                    bool truncate = vex_opcode == 0x2c;
                    if (is_sd) out->opcode = truncate ? HB_INS_CVTTSD2SI : HB_INS_CVTSD2SI;
                    else out->opcode = truncate ? HB_INS_CVTTSS2SI : HB_INS_CVTSS2SI;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                is_sd ? 8 : 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    out->op1.size = vex_w ? 8 : 4;
                    mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = is_sd ? 8 : 4;
                    return HB_OK;
                }
                if ((vex_opcode == 0x52 || vex_opcode == 0x53) && vex_pp == 2) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x52 ? HB_INS_RSQRTSS : HB_INS_RCPSS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, 16);
                    else if (out->op3.is_mem) out->op3.size = 4;
                    return HB_OK;
                }
                if ((vex_opcode == 0x12 || vex_opcode == 0x13 ||
                     vex_opcode == 0x16 || vex_opcode == 0x17) &&
                    (vex_pp == 0 || vex_pp == 1) && !vex_l) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t mod = (modrm >> 6) & 3;
                    if (mod == 3) {
                        if (vex_pp != 0 || vex_opcode == 0x13 || vex_opcode == 0x17)
                            return HB_ERR_UNSUPPORTED_OPCODE;
                        out->opcode = vex_opcode == 0x12 ? HB_INS_VMOVHLPS : HB_INS_VMOVLHPS;
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    16, out, 1, 2, false);
                        if (r != HB_OK) return r;
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_vec_operand(out, 1, 16);
                        mark_vec_operand(out, 2, 16);
                        mark_vec_operand(out, 3, 16);
                        return HB_OK;
                    }
                    bool store = vex_opcode == 0x13 || vex_opcode == 0x17;
                    bool high = vex_opcode == 0x16 || vex_opcode == 0x17;
                    if (store && vex_v != 0) return HB_ERR_UNSUPPORTED_OPCODE;
                    if (vex_pp == 1) out->opcode = high ? HB_INS_VMOVHPD : HB_INS_VMOVLPD;
                    else out->opcode = high ? HB_INS_VMOVHPS : HB_INS_VMOVLPS;
                    out->writes_flags = false;
                    if (store) {
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    8, out, 1, 2, true);
                        if (r != HB_OK) return r;
                        mark_xmm_operand(out, 2);
                        out->op1.size = 8;
                        return HB_OK;
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                8, out, 1, 3, false);
                    if (r != HB_OK) return r;
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_mem) out->op3.size = 8;
                    return HB_OK;
                }
                if ((vex_opcode == 0x10 || vex_opcode == 0x11) &&
                    (vex_pp == 2 || vex_pp == 3)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t lane = vex_pp == 3 ? 8 : 4;
                    out->opcode = HB_INS_SSE_MOV;
                    if (vex_opcode == 0x10) {
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    lane, out, 1, 2, false);
                        if (r != HB_OK) return r;
                        out->op1.size = 16;
                        mark_xmm_operand(out, 1);
                        if (mod == 3) {
                            out->op3 = out->op2;
                            if (out->op3.is_reg) out->op3.reg += HB_REG_XMM0;
                            memset(&out->op2, 0, sizeof(out->op2));
                            set_reg(out, 2, HB_REG_XMM0 + vex_v, 16);
                        }
                        return HB_OK;
                    }
                    if (mod == 3) {
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    lane, out, 3, 1, false);
                        if (r != HB_OK) return r;
                        out->op1.size = 16;
                        mark_xmm_operand(out, 1);
                        if (out->op3.is_reg) out->op3.reg += HB_REG_XMM0;
                        set_reg(out, 2, HB_REG_XMM0 + vex_v, 16);
                    } else {
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    lane, out, 1, 2, true);
                        if (r != HB_OK) return r;
                        if (out->op2.is_reg) out->op2.reg += HB_REG_XMM0;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0xc6) && (vex_pp == 0 || vex_pp == 1)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_pp == 1 ? HB_INS_SHUFPD : HB_INS_SHUFPS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, vec_size);
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, vec_size);
                    else if (out->op3.is_mem) out->op3.size = vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                    return HB_OK;
                }
                if (vex_pp == 1 && (vex_opcode == 0x71 || vex_opcode == 0x72 || vex_opcode == 0x73)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t ext = (modrm >> 3) & 7;
                    if (vex_opcode == 0x71 && ext == 2) mapped = HB_INS_PSRLW;
                    else if (vex_opcode == 0x71 && ext == 4) mapped = HB_INS_PSRAW;
                    else if (vex_opcode == 0x71 && ext == 6) mapped = HB_INS_PSLLW;
                    else if (vex_opcode == 0x72 && ext == 2) mapped = HB_INS_PSRLD;
                    else if (vex_opcode == 0x72 && ext == 4) mapped = HB_INS_PSRAD;
                    else if (vex_opcode == 0x72 && ext == 6) mapped = HB_INS_PSLLD;
                    else if (vex_opcode == 0x73 && ext == 2) mapped = HB_INS_PSRLQ;
                    else if (vex_opcode == 0x73 && ext == 3) mapped = HB_INS_PSRLDQ;
                    else if (vex_opcode == 0x73 && ext == 6) mapped = HB_INS_PSLLQ;
                    else if (vex_opcode == 0x73 && ext == 7) mapped = HB_INS_PSLLDQ;
                    else return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = mapped;
                    out->writes_flags = false;
                    set_reg(out, 1, vex_v, vec_size);
                    hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, vec_size, out, 2);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (out->op2.is_mem) out->op2.size = vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_imm(out, 3, read_u8(d), 1);
                    return HB_OK;
                }
                switch (vex_opcode) {
                    case 0x14:
                        if (vex_pp == 0 || vex_pp == 1) mapped = vex_pp == 1 ? HB_INS_UNPCKLPD : HB_INS_UNPCKLPS;
                        break;
                    case 0x15:
                        if (vex_pp == 0 || vex_pp == 1) mapped = vex_pp == 1 ? HB_INS_UNPCKHPD : HB_INS_UNPCKHPS;
                        break;
                    case 0x7c:
                        if (vex_pp == 1 || vex_pp == 3) mapped = vex_pp == 1 ? HB_INS_VHADDPD : HB_INS_VHADDPS;
                        break;
                    case 0x7d:
                        if (vex_pp == 1 || vex_pp == 3) mapped = vex_pp == 1 ? HB_INS_VHSUBPD : HB_INS_VHSUBPS;
                        break;
                    case 0x54: mapped = HB_INS_XMM_AND; break;
                    case 0x55: mapped = HB_INS_XMM_ANDN; break;
                    case 0x56: mapped = HB_INS_XMM_OR; break;
                    case 0x57: mapped = HB_INS_XORPS; break;
                    case 0x51:
                        mapped = vex_pp == 1 ? HB_INS_SQRTPD : (vex_pp == 2 ? HB_INS_SQRTSS : (vex_pp == 3 ? HB_INS_SQRTSD : HB_INS_SQRTPS));
                        break;
                    case 0x52:
                        if (vex_pp == 0) mapped = HB_INS_RSQRTPS;
                        break;
                    case 0x53:
                        if (vex_pp == 0) mapped = HB_INS_RCPPS;
                        break;
                    case 0x58:
                        mapped = vex_pp == 1 ? HB_INS_ADDPD : (vex_pp == 2 ? HB_INS_ADDSS : (vex_pp == 3 ? HB_INS_ADDSD : HB_INS_ADDPS));
                        break;
                    case 0x59:
                        mapped = vex_pp == 1 ? HB_INS_MULPD : (vex_pp == 2 ? HB_INS_MULSS : (vex_pp == 3 ? HB_INS_MULSD : HB_INS_MULPS));
                        break;
                    case 0x5c:
                        mapped = vex_pp == 1 ? HB_INS_SUBPD : (vex_pp == 2 ? HB_INS_SUBSS : (vex_pp == 3 ? HB_INS_SUBSD : HB_INS_SUBPS));
                        break;
                    case 0x5d:
                        mapped = vex_pp == 1 ? HB_INS_MINPD : (vex_pp == 2 ? HB_INS_MINSS : (vex_pp == 3 ? HB_INS_MINSD : HB_INS_MINPS));
                        break;
                    case 0x5e:
                        mapped = vex_pp == 1 ? HB_INS_DIVPD : (vex_pp == 2 ? HB_INS_DIVSS : (vex_pp == 3 ? HB_INS_DIVSD : HB_INS_DIVPS));
                        break;
                    case 0x5f:
                        mapped = vex_pp == 1 ? HB_INS_MAXPD : (vex_pp == 2 ? HB_INS_MAXSS : (vex_pp == 3 ? HB_INS_MAXSD : HB_INS_MAXPS));
                        break;
                    case 0xd0:
                        if (vex_pp == 1 || vex_pp == 3) mapped = vex_pp == 1 ? HB_INS_VADDSUBPD : HB_INS_VADDSUBPS;
                        break;
                    case 0xdb: mapped = HB_INS_XMM_AND; break;
                    case 0xdf: mapped = HB_INS_XMM_ANDN; break;
                    case 0xeb: mapped = HB_INS_XMM_OR; break;
                    case 0xef: mapped = HB_INS_PXOR; break;
                    case 0xfc: mapped = HB_INS_PADDB; break;
                    case 0xfd: mapped = HB_INS_PADDW; break;
                    case 0xfe: mapped = HB_INS_PADDD; break;
                    case 0xd4: mapped = HB_INS_PADDQ; break;
                    case 0xf8: mapped = HB_INS_PSUBB; break;
                    case 0xf9: mapped = HB_INS_PSUBW; break;
                    case 0xfa: mapped = HB_INS_PSUBD; break;
                    case 0xfb: mapped = HB_INS_PSUBQ; break;
                    default: break;
                }
                if (!mapped && vex_pp == 1 && vex_opcode != 0xc4 && vex_opcode != 0xc5) {
                    mapped = sse2_0f_packed_opcode(vex_opcode);
                }
                if (!mapped && vex_pp == 1) {
                    mapped = sse2_0f_unpack_pack_opcode(vex_opcode);
                }
                if (!mapped && vex_pp == 1) {
                    mapped = sse2_0f_compare_opcode(vex_opcode);
                }
            } else if (vex_map == 2 && vex_pp == 1) {
                if ((vex_opcode == 0x0e || vex_opcode == 0x0f) && !vex_w) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x0f ? HB_INS_VTESTPD : HB_INS_VTESTPS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                    else if (out->op2.is_mem) out->op2.size = vec_size;
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x18 || vex_opcode == 0x19 ||
                                vex_opcode == 0x1a)) {
                    uint8_t src_bytes = 0;
                    bool allow_reg_src = false;
                    if (vex_opcode == 0x18 && !vex_w) {
                        mapped = HB_INS_VBROADCASTSS;
                        src_bytes = 4;
                        allow_reg_src = true;
                    } else if (vex_opcode == 0x19 && !vex_w && vex_l) {
                        mapped = HB_INS_VBROADCASTSD;
                        src_bytes = 8;
                        allow_reg_src = true;
                    } else if (vex_opcode == 0x1a && !vex_w && vex_l) {
                        mapped = HB_INS_VBROADCASTF32X4;
                        src_bytes = 16;
                    } else {
                        return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if (((modrm >> 6) & 3u) == 3 && !allow_reg_src) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = mapped;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                src_bytes, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = src_bytes;
                    return HB_OK;
                }
                if (vex_opcode == 0x13 && !vex_w) {
                    uint8_t src_bytes = vex_l ? 16 : 8;
                    uint8_t dst_bytes = vex_l ? 32 : 16;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_VCVTPH2PS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                src_bytes, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, dst_bytes);
                    if (out->op2.is_reg) {
                        out->op2.reg += HB_REG_XMM0;
                        out->op2.size = src_bytes;
                    } else if (out->op2.is_mem) {
                        out->op2.size = src_bytes;
                    }
                    return HB_OK;
                }
                if (vex_opcode == 0x2a) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    out->opcode = HB_INS_SSE_MOV;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, vec_size);
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x0c || vex_opcode == 0x0d) && !vex_w) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x0c ? HB_INS_VPERMILPS : HB_INS_VPERMILPD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, vec_size);
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, vec_size);
                    else if (out->op3.is_mem) out->op3.size = vec_size;
                    return HB_OK;
                }
                if (!mapped) mapped = fma3_0f38_opcode(vex_opcode, vex_w);
                if (mapped && mapped >= HB_INS_VFMADD132PS && mapped <= HB_INS_VFNMSUB231SD) {
                    bool scalar = fma3_opcode_is_scalar(vex_opcode);
                    uint8_t lane = vex_w ? 8 : 4;
                    uint8_t op_bytes = scalar ? 16 : vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = mapped;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                op_bytes, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, op_bytes);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, op_bytes);
                    else if (out->op3.is_mem) out->op3.size = scalar ? lane : op_bytes;
                    return HB_OK;
                }
                mapped = ssse3_0f38_opcode(vex_opcode);
                if (!mapped && vex_opcode == 0xcf) mapped = HB_INS_VGF2P8MULB;
                if (!mapped && vex_opcode == 0xdb) mapped = HB_INS_AESIMC;
                if (!mapped && vex_opcode >= 0xdc && vex_opcode <= 0xdf) {
                    switch (vex_opcode) {
                        case 0xdc: mapped = HB_INS_VAESENC; break;
                        case 0xdd: mapped = HB_INS_VAESENCLAST; break;
                        case 0xde: mapped = HB_INS_VAESDEC; break;
                        case 0xdf: mapped = HB_INS_VAESDECLAST; break;
                        default: break;
                    }
                }
                if (!mapped) mapped = sse41_0f38_opcode(vex_opcode);
                if (!mapped && vex_opcode >= 0x90 && vex_opcode <= 0x93) {
                    uint8_t elem = 4, index = 4, count = 0, dest_size = vec_size;
                    if (vex_opcode == 0x90) {
                        out->opcode = vex_w ? HB_INS_VPGATHERDQ : HB_INS_VPGATHERDD;
                        elem = vex_w ? 8 : 4;
                        index = 4;
                        count = (uint8_t)(dest_size / elem);
                    } else if (vex_opcode == 0x91) {
                        out->opcode = vex_w ? HB_INS_VPGATHERQQ : HB_INS_VPGATHERQD;
                        elem = vex_w ? 8 : 4;
                        index = 8;
                        if (!vex_w) {
                            count = vex_l ? 4 : 2;
                            dest_size = 16;
                        } else {
                            count = (uint8_t)(dest_size / elem);
                        }
                    } else if (vex_opcode == 0x92) {
                        out->opcode = vex_w ? HB_INS_VGATHERDPD : HB_INS_VGATHERDPS;
                        elem = vex_w ? 8 : 4;
                        index = 4;
                        count = (uint8_t)(dest_size / elem);
                    } else {
                        out->opcode = vex_w ? HB_INS_VGATHERQPD : HB_INS_VGATHERQPS;
                        elem = vex_w ? 8 : 4;
                        index = 8;
                        if (!vex_w) {
                            count = vex_l ? 4 : 2;
                            dest_size = 16;
                        } else {
                            count = (uint8_t)(dest_size / elem);
                        }
                    }
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    hb_result_t r = parse_vsib_modrm(d, modrm, rex_r, rex_x, rex_b,
                                                     dest_size, out, 1, 3,
                                                     index, elem, count);
                    if (r != HB_OK) return r;
                    set_reg(out, 2, vex_v, dest_size);
                    mark_vec_operand(out, 1, dest_size);
                    mark_vec_operand(out, 2, dest_size);
                    if (out->op3.is_mem) out->op3.size = dest_size;
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x2c || vex_opcode == 0x2d ||
                                vex_opcode == 0x2e || vex_opcode == 0x2f ||
                                vex_opcode == 0x8c || vex_opcode == 0x8e)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t mod = (modrm >> 6) & 3;
                    if (mod == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                    bool store = vex_opcode == 0x2e || vex_opcode == 0x2f || vex_opcode == 0x8e;
                    uint8_t lane = (vex_opcode == 0x2d || vex_opcode == 0x2f ||
                                    (vex_opcode >= 0x8c && vex_w)) ? 8 : 4;
                    if (vex_opcode == 0x2c || vex_opcode == 0x2e) out->opcode = HB_INS_VMASKMOVPS;
                    else if (vex_opcode == 0x2d || vex_opcode == 0x2f) out->opcode = HB_INS_VMASKMOVPD;
                    else out->opcode = vex_w ? HB_INS_VPMASKMOVQ : HB_INS_VPMASKMOVD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, store);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    if (store) {
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, vec_size);
                        mark_vec_operand(out, 2, vec_size);
                        mark_vec_operand(out, 3, vec_size);
                        if (out->op1.is_mem) out->op1.size = vec_size;
                    } else {
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, vec_size);
                        mark_vec_operand(out, 1, vec_size);
                        mark_vec_operand(out, 2, vec_size);
                        if (out->op3.is_mem) out->op3.size = vec_size;
                    }
                    out->imm8 = lane;
                    out->has_imm8 = true;
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x16 || vex_opcode == 0x36) && !vex_w && vex_l) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x16 ? HB_INS_VPERMPS : HB_INS_VPERMD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                32, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 32);
                    mark_vec_operand(out, 1, 32);
                    mark_vec_operand(out, 2, 32);
                    mark_vec_operand(out, 3, 32);
                    if (out->op3.is_mem) out->op3.size = 32;
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x45 || vex_opcode == 0x46 ||
                                vex_opcode == 0x47)) {
                    if (vex_opcode == 0x45) {
                        mapped = vex_w ? HB_INS_VPSRLVQ : HB_INS_VPSRLVD;
                    } else if (vex_opcode == 0x46) {
                        if (vex_w) return HB_ERR_UNSUPPORTED_OPCODE;
                        mapped = HB_INS_VPSRAVD;
                    } else {
                        mapped = vex_w ? HB_INS_VPSLLVQ : HB_INS_VPSLLVD;
                    }
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = mapped;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, vec_size);
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    mark_vec_operand(out, 3, vec_size);
                    if (out->op3.is_mem) out->op3.size = vec_size;
                    return HB_OK;
                }
                if (!mapped && (vex_opcode == 0x58 || vex_opcode == 0x59 ||
                                vex_opcode == 0x5a || vex_opcode == 0x78 ||
                                vex_opcode == 0x79)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    uint8_t mod = (modrm >> 6) & 3;
                    uint8_t lane = 0;
                    if (vex_opcode == 0x78) {
                        mapped = HB_INS_VPBROADCASTB;
                        lane = 1;
                    } else if (vex_opcode == 0x79) {
                        mapped = HB_INS_VPBROADCASTW;
                        lane = 2;
                    } else if (vex_opcode == 0x58) {
                        mapped = HB_INS_VPBROADCASTD;
                        lane = 4;
                    } else if (vex_opcode == 0x59) {
                        mapped = HB_INS_VPBROADCASTQ;
                        lane = 8;
                    } else {
                        if (!vex_l || mod == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                        mapped = HB_INS_VBROADCASTI128;
                        lane = 16;
                    }
                    out->opcode = mapped;
                    out->writes_flags = false;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                lane, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operand(out, 1, vex_opcode == 0x5a ? 32 : vec_size);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    if (out->op2.is_mem) out->op2.size = lane;
                    return HB_OK;
                }
            } else if (vex_map == 3 && vex_pp == 1) {
                if ((vex_opcode == 0x04 || vex_opcode == 0x05) && !vex_w) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x04 ? HB_INS_VPERMILPS : HB_INS_VPERMILPD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    mark_vec_operand(out, 1, vec_size);
                    if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                    else if (out->op2.is_mem) out->op2.size = vec_size;
                    set_imm(out, 3, imm, 1);
                    set_extra_imm8(out, imm);
                    return HB_OK;
                }
                if (vex_opcode == 0x1d && !vex_w) {
                    uint8_t result_bytes = vex_l ? 16 : 8;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_VCVTPS2PH;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, true);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    if (out->op1.is_reg) {
                        out->op1.reg += HB_REG_XMM0;
                        out->op1.size = 16;
                    } else if (out->op1.is_mem) {
                        out->op1.size = result_bytes;
                    }
                    mark_vec_operand(out, 2, vec_size);
                    return HB_OK;
                }
                if (vex_opcode >= 0x08 && vex_opcode <= 0x0b && vex_pp == 1) {
                    bool scalar = vex_opcode == 0x0a || vex_opcode == 0x0b;
                    uint8_t lane = (vex_opcode == 0x09 || vex_opcode == 0x0b) ? 8 : 4;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    switch (vex_opcode) {
                        case 0x08: out->opcode = HB_INS_ROUNDPS; break;
                        case 0x09: out->opcode = HB_INS_ROUNDPD; break;
                        case 0x0a: out->opcode = HB_INS_ROUNDSS; break;
                        case 0x0b: out->opcode = HB_INS_ROUNDSD; break;
                        default: return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                scalar ? lane : vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    if (scalar) {
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_vec_operand(out, 1, 16);
                        mark_vec_operand(out, 2, 16);
                        if (out->op3.is_reg) mark_vec_operand(out, 3, 16);
                        else if (out->op3.is_mem) out->op3.size = lane;
                    } else {
                        mark_vec_operand(out, 1, vec_size);
                        if (out->op2.is_reg) mark_vec_operand(out, 2, vec_size);
                        else if (out->op2.is_mem) out->op2.size = vec_size;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0x40 || vex_opcode == 0x41) && vex_pp == 1 && !vex_w) {
                    bool fp64 = vex_opcode == 0x41;
                    if (fp64 && vex_l) return HB_ERR_UNSUPPORTED_OPCODE;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = fp64 ? HB_INS_DPPD : HB_INS_DPPS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                fp64 ? 16 : vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, fp64 ? 16 : vec_size);
                    mark_vec_operand(out, 1, fp64 ? 16 : vec_size);
                    mark_vec_operand(out, 2, fp64 ? 16 : vec_size);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, fp64 ? 16 : vec_size);
                    else if (out->op3.is_mem) out->op3.size = fp64 ? 16 : vec_size;
                    return HB_OK;
                }
                if (vex_opcode == 0x42 && vex_pp == 1 && !vex_w && !vex_l) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_VMPSADBW;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, 16);
                    else if (out->op3.is_mem) out->op3.size = 16;
                    return HB_OK;
                }
                if ((vex_opcode == 0x17 || vex_opcode == 0x21) && vex_pp == 1 && !vex_l) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x17 ? HB_INS_EXTRACTPS : HB_INS_INSERTPS;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2,
                                                vex_opcode == 0x17);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    if (vex_opcode == 0x17) {
                        mark_xmm_operand(out, 2);
                        if (out->op1.is_mem) out->op1.size = 4;
                    } else {
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_xmm_operand(out, 1);
                        mark_xmm_operand(out, 2);
                        if (out->op3.is_reg) mark_xmm_operand(out, 3);
                        else if (out->op3.is_mem) out->op3.size = 4;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0x14 || vex_opcode == 0x15 || vex_opcode == 0x16 ||
                     vex_opcode == 0x20 || vex_opcode == 0x22) && vex_pp == 1 && !vex_l) {
                    bool extract = vex_opcode == 0x14 || vex_opcode == 0x15 || vex_opcode == 0x16;
                    uint8_t elem_size = 4;
                    if (vex_opcode == 0x14 || vex_opcode == 0x20) elem_size = 1;
                    else if (vex_opcode == 0x15) elem_size = 2;
                    else if (vex_w) elem_size = 8;
                    if ((vex_opcode == 0x14 || vex_opcode == 0x15 || vex_opcode == 0x20) && vex_w) {
                        return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    if (extract) {
                        if (vex_opcode == 0x14) out->opcode = HB_INS_PEXTRB;
                        else if (vex_opcode == 0x15) out->opcode = HB_INS_PEXTRW;
                        else out->opcode = vex_w ? HB_INS_PEXTRQ : HB_INS_PEXTRD;
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    elem_size == 8 ? 8 : 4, out, 1, 2, true);
                        if (r != HB_OK) return r;
                        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                        set_extra_imm8(out, read_u8(d));
                        mark_xmm_operand(out, 2);
                        if (out->op1.is_mem) out->op1.size = elem_size;
                    } else {
                        if (vex_opcode == 0x20) out->opcode = HB_INS_PINSRB;
                        else out->opcode = vex_w ? HB_INS_PINSRQ : HB_INS_PINSRD;
                        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                    elem_size == 8 ? 8 : 4, out, 1, 2, false);
                        if (r != HB_OK) return r;
                        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                        uint8_t imm = read_u8(d);
                        set_extra_imm8(out, imm);
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v, 16);
                        mark_xmm_operand(out, 1);
                        mark_xmm_operand(out, 2);
                        if (out->op3.is_mem) out->op3.size = elem_size;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0xce || vex_opcode == 0xcf) && vex_pp == 1 && vex_w) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0xce ? HB_INS_VGF2P8AFFINEQB : HB_INS_VGF2P8AFFINEINVQB;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, vec_size);
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, vec_size);
                    else if (out->op3.is_mem) out->op3.size = vec_size;
                    return HB_OK;
                }
                if (vex_opcode == 0x44) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = HB_INS_VPCLMULQDQ;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                16, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, 16);
                    mark_vec_operand(out, 1, 16);
                    mark_vec_operand(out, 2, 16);
                    mark_vec_operand(out, 3, 16);
                    if (out->op3.is_mem) out->op3.size = 16;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    return HB_OK;
                }
                if ((vex_opcode == 0x00 && vex_w && vex_l) ||
                    (vex_opcode == 0x01 && vex_w && vex_l)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    out->opcode = vex_opcode == 0x00 ? HB_INS_VPERMQ : HB_INS_VPERMPD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                32, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    mark_vec_operands(out, 32);
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_imm(out, 3, imm, 1);
                    set_extra_imm8(out, imm);
                    return HB_OK;
                }
                if ((vex_opcode == 0x02 && !vex_w) ||
                    (vex_opcode == 0x06 && vex_l) ||
                    (vex_opcode == 0x18 && vex_l) ||
                    (vex_opcode == 0x19 && vex_l) ||
                    (vex_opcode == 0x38 && vex_l) ||
                    (vex_opcode == 0x39 && vex_l) ||
                    (vex_opcode == 0x46 && vex_l)) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    bool extract = vex_opcode == 0x19 || vex_opcode == 0x39;
                    uint8_t rm_size = (vex_opcode == 0x18 || vex_opcode == 0x38 || extract) ? 16 : vec_size;
                    switch (vex_opcode) {
                        case 0x02: out->opcode = HB_INS_VPBLENDD; break;
                        case 0x06: out->opcode = HB_INS_VPERM2F128; break;
                        case 0x18: out->opcode = HB_INS_VINSERTF128; break;
                        case 0x19: out->opcode = HB_INS_VEXTRACTF128; break;
                        case 0x38: out->opcode = HB_INS_VINSERTI128; break;
                        case 0x39: out->opcode = HB_INS_VEXTRACTI128; break;
                        case 0x46: out->opcode = HB_INS_VPERM2I128; break;
                        default: return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                rm_size, out, 1, 2, extract);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t imm = read_u8(d);
                    set_extra_imm8(out, imm);
                    if (extract) {
                        if (out->op1.is_reg) mark_vec_operand(out, 1, 16);
                        else if (out->op1.is_mem) out->op1.size = 16;
                        mark_vec_operand(out, 2, 32);
                    } else {
                        hb_decoded_t tmp = *out;
                        out->op3 = tmp.op2;
                        memset(&out->op2, 0, sizeof(out->op2));
                        set_reg(out, 2, vex_v + HB_REG_XMM0,
                                (vex_opcode == 0x18 || vex_opcode == 0x38 ||
                                 vex_opcode == 0x06 || vex_opcode == 0x46) ? 32 : vec_size);
                        mark_vec_operand(out, 1,
                                         (vex_opcode == 0x18 || vex_opcode == 0x38 ||
                                          vex_opcode == 0x06 || vex_opcode == 0x46) ? 32 : vec_size);
                        if (out->op3.is_reg) mark_vec_operand(out, 3, rm_size);
                        else if (out->op3.is_mem) out->op3.size = rm_size;
                    }
                    return HB_OK;
                }
                if ((vex_opcode == 0x4a || vex_opcode == 0x4b || vex_opcode == 0x4c) && !vex_w) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    uint8_t modrm = read_u8(d);
                    switch (vex_opcode) {
                        case 0x4a: out->opcode = HB_INS_VBLENDVPS; break;
                        case 0x4b: out->opcode = HB_INS_VBLENDVPD; break;
                        case 0x4c: out->opcode = HB_INS_VPBLENDVB; break;
                        default: return HB_ERR_UNSUPPORTED_OPCODE;
                    }
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                vec_size, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    hb_decoded_t tmp = *out;
                    out->op3 = tmp.op2;
                    memset(&out->op2, 0, sizeof(out->op2));
                    set_reg(out, 2, vex_v, vec_size);
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (out->op3.is_reg) mark_vec_operand(out, 3, vec_size);
                    else if (out->op3.is_mem) out->op3.size = vec_size;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                    return HB_OK;
                }
                mapped = sse41_0f3a_opcode(vex_opcode);
            }
            if (mapped) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = mapped;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, vec_size, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (vex_map == 3) {
                    mark_vec_operand(out, 1, vec_size);
                    mark_vec_operand(out, 2, vec_size);
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_imm(out, 3, read_u8(d), 1);
                    return HB_OK;
                }
                hb_decoded_t tmp = *out;
                out->op3 = tmp.op2;
                memset(&out->op2, 0, sizeof(out->op2));
                set_reg(out, 2, vex_v, vec_size);
                mark_vec_operand(out, 1, vec_size);
                mark_vec_operand(out, 2, vec_size);
                mark_vec_operand(out, 3, vec_size);
                if (vex_map == 1 && vex_pp == 1 && sse2_0f_variable_shift_opcode(vex_opcode)) {
                    out->op3.size = 16;
                }
                return HB_OK;
            }
        }
        out->opcode = HB_INS_VEC;
        if (vex_opcode == 0x77) return HB_OK;
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
        if (r != HB_OK) return r;
        mark_xmm_operand(out, 1);
        mark_xmm_operand(out, 2);
        if (vex_opcode == 0xc2) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
        }
        return HB_OK;
    }

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
    if (opcode == 0x8C || opcode == 0x8E) {
        /* MOV r/m16,Sreg and MOV Sreg,r/m16.  REX.W promotes register
           operands to r64 in long mode; memory selector slots stay 16-bit. */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t seg = (modrm >> 3) & 7;
        hb_result_t r;
        if (seg > 5) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_MOV_SEG;
        out->writes_flags = false;
        if (opcode == 0x8C) {
            r = parse_modrm_ext(d, modrm, rex_w, rex_b, 2, out, 1);
            if (r != HB_OK) return r;
            if (out->op1.is_mem) out->op1.size = 2;
            set_imm(out, 2, seg, 2);
        } else {
            set_imm(out, 1, seg, 2);
            r = parse_modrm_ext(d, modrm, rex_w, rex_b, 2, out, 2);
            if (r != HB_OK) return r;
            if (out->op2.is_mem) out->op2.size = 2;
        }
        return HB_OK;
    }
    if (opcode == 0x8F) {
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (((modrm >> 3) & 7) != 0) return HB_ERR_UNSUPPORTED_OPCODE;
        out->opcode = HB_INS_POP;
        out->stack_delta = 8;
        return parse_modrm_ext(d, modrm, rex_w, rex_b, rex_w ? 8 : (operand16 ? 2 : 8), out, 1);
    }
    if (opcode == 0x8D) {
        /* LEA r32/64, m */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        out->opcode = HB_INS_LEA;
        out->writes_flags = false;
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
    }
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        /* MOV AL/rAX, moffs and MOV moffs, AL/rAX. */
        uint8_t sz = (opcode == 0xA0 || opcode == 0xA2) ? 1 : op_size;
        uint64_t moffs;
        if (address32) {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            moffs = (uint32_t)read_s32(d);
        } else {
            if (!can_read(d, 8)) return HB_ERR_DECODE_FAILED;
            moffs = read_u64(d);
        }
        out->opcode = HB_INS_MOV;
        out->writes_flags = false;
        if (opcode == 0xA0 || opcode == 0xA1) {
            set_reg(out, 1, HB_REG_RAX, sz);
            set_mem(out, 2, -1, -1, 1, (int64_t)moffs, sz);
        } else {
            set_mem(out, 1, -1, -1, 1, (int64_t)moffs, sz);
            set_reg(out, 2, HB_REG_RAX, sz);
        }
        return HB_OK;
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
        if (modrm == 0xf8) {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            out->opcode = HB_INS_SYS;
            out->writes_flags = false;
            set_imm(out, 1, (int64_t)read_s32(d), 4);
            return HB_OK;
        }
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
        out->stack_delta = operand16 ? -2 : -8;
        if (operand16) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 1, (int64_t)read_s16(d), 2);
        } else {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 1, (int64_t)read_s32(d), 4);
        }
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
        hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                    operand16 ? 2 : 4, out, 1, 2, false);
        if (r != HB_OK) return r;
        if (opcode == 0x69) {
            if (out->op1.size == 2) {
                if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, (int64_t)read_s16(d), out->op1.size);
            } else {
                if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, (int64_t)read_s32(d), out->op1.size);
            }
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
    if (opcode == 0x6C || opcode == 0x6D) {
        /* INS m8/m16/m32, DX. Port I/O remains unsupported by the lifter. */
        out->opcode = HB_INS_INS;
        uint8_t sz = (opcode == 0x6C) ? 1 : (operand16 ? 2 : 4);
        set_reg(out, 1, HB_REG_RDI, sz);
        set_reg(out, 2, HB_REG_RDX, 2);
        set_imm(out, 3, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        return HB_OK;
    }
    if (opcode == 0x6E || opcode == 0x6F) {
        /* OUTS DX, m8/m16/m32. Port I/O remains unsupported by the lifter. */
        out->opcode = HB_INS_OUTS;
        uint8_t sz = (opcode == 0x6E) ? 1 : (operand16 ? 2 : 4);
        set_reg(out, 1, HB_REG_RDX, 2);
        set_reg(out, 2, HB_REG_RSI, sz);
        set_imm(out, 3, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        return HB_OK;
    }
    if (opcode == 0xA4 || opcode == 0xA5) {
        /* MOVS m8/m16/m32/m64. REP is carried as an immediate mode. */
        out->opcode = HB_INS_MOVS;
        uint8_t sz = (opcode == 0xA4) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        set_reg(out, 1, HB_REG_RSI, sz);
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
        return HB_OK;
    }
    if (opcode == 0xA6 || opcode == 0xA7) {
        /* CMPS m8/m16/m32/m64. REPNE/REPE are carried as an immediate mode. */
        out->opcode = HB_INS_CMPS;
        uint8_t sz = (opcode == 0xA6) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        set_reg(out, 1, HB_REG_RSI, sz);
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->reads_flags = true;
        out->writes_flags = true;
        return HB_OK;
    }
    if (opcode == 0xAC || opcode == 0xAD) {
        /* LODS m8/m16/m32/m64. REP is carried as an immediate mode. */
        out->opcode = HB_INS_LODS;
        uint8_t sz = (opcode == 0xAC) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
        set_reg(out, 1, HB_REG_RAX, sz);
        set_imm(out, 2, prefix_f2 ? 0xf2 : prefix_f3 ? 0xf3 : 0, 1);
        out->writes_flags = false;
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
    if (opcode == 0xD7) {
        out->opcode = HB_INS_XLAT;
        set_reg(out, 1, HB_REG_RAX, 1);
        set_mem(out, 2, HB_REG_RBX, HB_REG_RAX, 1, 0, 1);
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
        out->opcode = opcode == 0xE3 ? HB_INS_JRCXZ : HB_INS_LOOP;
        out->is_branch = true;
        out->is_conditional = true;
        out->branch_target = addr + d->pos + rel;
        out->reads_flags = opcode == 0xE0 || opcode == 0xE1;
        set_reg(out, 1, HB_REG_RCX, address32 ? 4 : 8);
        set_imm(out, 2, opcode - 0xE0, 1);
        return HB_OK;
    }
    if (opcode == 0xE4 || opcode == 0xE5 || opcode == 0xEC || opcode == 0xED) {
        out->opcode = HB_INS_IN;
        uint8_t sz = (opcode == 0xE4 || opcode == 0xEC) ? 1 : (operand16 ? 2 : 4);
        set_reg(out, 1, HB_REG_RAX, sz);
        if (opcode == 0xE4 || opcode == 0xE5) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_u8(d), 1);
        } else {
            set_reg(out, 2, HB_REG_RDX, 2);
        }
        return HB_OK;
    }
    if (opcode == 0xE6 || opcode == 0xE7 || opcode == 0xEE || opcode == 0xEF) {
        out->opcode = HB_INS_OUT;
        uint8_t sz = (opcode == 0xE6 || opcode == 0xEE) ? 1 : (operand16 ? 2 : 4);
        if (opcode == 0xE6 || opcode == 0xE7) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 1, read_u8(d), 1);
        } else {
            set_reg(out, 1, HB_REG_RDX, 2);
        }
        set_reg(out, 2, HB_REG_RAX, sz);
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
    if (opcode == 0xCB || opcode == 0xCA) {
        out->opcode = HB_INS_RETF;
        out->is_ret = true;
        out->stack_delta = 16;
        if (opcode == 0xCA) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            out->ret_imm = (uint16_t)(read_u8(d) | (read_u8(d) << 8));
        }
        return HB_OK;
    }
    if (opcode == 0xC8) {
        out->opcode = HB_INS_ENTER;
        out->stack_delta = -8;
        if (!can_read(d, 3)) return HB_ERR_DECODE_FAILED;
        uint16_t alloc = (uint16_t)(read_u8(d) | (read_u8(d) << 8));
        uint8_t nesting = read_u8(d);
        set_imm(out, 1, alloc, 2);
        set_imm(out, 2, nesting, 1);
        return HB_OK;
    }
    if (opcode == 0xC9) {
        out->opcode = HB_INS_LEAVE;
        out->writes_flags = false;
        out->stack_delta = operand16 ? 2 : 8;
        set_reg(out, 1, HB_REG_RBP, operand16 ? 2 : 8);
        return HB_OK;
    }
    if (opcode == 0xCC || opcode == 0xCD || opcode == 0xCE || opcode == 0xCF || opcode == 0xF1) {
        if (opcode == 0xCC) out->opcode = HB_INS_INT3;
        else if (opcode == 0xCD) out->opcode = HB_INS_INT;
        else if (opcode == 0xCE) out->opcode = HB_INS_INTO;
        else if (opcode == 0xCF) out->opcode = HB_INS_IRET;
        else out->opcode = HB_INS_INT1;
        if (opcode == 0xCD) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 1, read_u8(d), 1);
        }
        out->is_branch = true;
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
    if (opcode == 0x9C || opcode == 0x9D) {
        out->opcode = opcode == 0x9C ? HB_INS_PUSHF : HB_INS_POPF;
        out->writes_flags = opcode == 0x9D;
        out->stack_delta = opcode == 0x9C ? -8 : 8;
        set_imm(out, 1, operand16 ? 2 : 8, 1);
        return HB_OK;
    }
    if (opcode == 0xF5 || opcode == 0xF8 || opcode == 0xF9 || opcode == 0xFA ||
        opcode == 0xFB || opcode == 0xFC || opcode == 0xFD) {
        if (opcode == 0xF5) out->opcode = HB_INS_CMC;
        else if (opcode == 0xF8) out->opcode = HB_INS_CLC;
        else if (opcode == 0xF9) out->opcode = HB_INS_STC;
        else if (opcode == 0xFA) out->opcode = HB_INS_CLI;
        else if (opcode == 0xFB) out->opcode = HB_INS_STI;
        else if (opcode == 0xFC) out->opcode = HB_INS_CLD;
        else out->opcode = HB_INS_STD;
        out->writes_flags = true;
        if (opcode == 0xF5) out->reads_flags = true;
        return HB_OK;
    }
    if (opcode == 0xF4) {
        out->opcode = HB_INS_HLT;
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

    /* Group: NOP / XCHG rAX, r16/32/64. */
    if (opcode >= 0x90 && opcode <= 0x97) {
        if (opcode == 0x90 && !rex_b) {
            out->opcode = HB_INS_NOP;
            return HB_OK;
        }
        out->opcode = HB_INS_XCHG;
        uint8_t sz = rex_w ? 8 : (operand16 ? 2 : 4);
        set_reg(out, 1, HB_REG_RAX, sz);
        set_reg(out, 2, reg_idx(opcode & 7, rex_b), sz);
        return HB_OK;
    }
    if (opcode == 0x9B) {
        /* FWAIT waits for pending x87 exceptions. HyperBridge does not model
           asynchronous x87 exceptions yet, so it is a serialization no-op. */
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
            out->opcode = HB_INS_SYS;
            if ((modrm >> 6) == 3) return HB_OK;
            return parse_modrm_ext(d, modrm, false, rex_b, 8, out, 1);
        }
        if (op2 == 0x00) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SYS;
            return parse_modrm_ext(d, modrm, false, rex_b, 2, out, 1);
        }
        if (op2 == 0x02 || op2 == 0x03) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SYS;
            return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
        }
        if (op2 == 0xA6 || op2 == 0xA7) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SYS;
            return parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        }
        if (op2 == 0x05 || op2 == 0x06 || op2 == 0x07 || op2 == 0x08 ||
            op2 == 0x09 || op2 == 0x0E || op2 == 0x30 || op2 == 0x31 ||
            op2 == 0x32 || op2 == 0x33 || op2 == 0x34 || op2 == 0x35 ||
            op2 == 0x37 || op2 == 0x77 || op2 == 0xAA) {
            out->opcode = (op2 == 0x0E || op2 == 0x77) ? HB_INS_MMX : HB_INS_SYS;
            return HB_OK;
        }
        if (op2 == 0x0B || op2 == 0xFF) {
            out->opcode = HB_INS_UD;
            return HB_OK;
        }
        if (op2 == 0xB9) {
            out->opcode = HB_INS_UD;
            return HB_OK;
        }
        if (op2 >= 0x18 && op2 <= 0x1E) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_NOP;
            return parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
        }
        if (!operand16 && !prefix_f2 && !prefix_f3 &&
            (op2 == 0x2A || op2 == 0x2C || op2 == 0x2D ||
            (op2 >= 0x60 && op2 <= 0x6B) || op2 == 0x6E ||
            op2 == 0x70 || (op2 >= 0x74 && op2 <= 0x76) ||
            op2 == 0x7E || op2 == 0xC4 || op2 == 0xC5 ||
            (op2 >= 0xD1 && op2 <= 0xD5) || (op2 >= 0xD7 && op2 <= 0xE5) ||
            (op2 >= 0xE8 && op2 <= 0xEF) || (op2 >= 0xF1 && op2 <= 0xF7) ||
            (op2 >= 0xF8 && op2 <= 0xFE))) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MMX;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            /* MacRunner: 0xC2 removed here — no-prefix 0F C2 is CMPPS (SSE, not MMX);
             * it now falls through to the generic_0f_vec path below. */
            if (op2 == 0x70 || op2 == 0xC4 || op2 == 0xC5) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
            }
            return HB_OK;
        }
        if (op2 == 0x20 || op2 == 0x22 || op2 == 0x21 || op2 == 0x23) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ctrl = (uint8_t)(((modrm >> 3) & 7) | (rex_r ? 8 : 0));
            out->opcode = (op2 == 0x20 || op2 == 0x22) ? HB_INS_MOV_CR : HB_INS_MOV_DR;
            if (op2 == 0x20 || op2 == 0x21) {
                set_reg(out, 1, reg_idx(modrm & 7, rex_b), 8);
                set_imm(out, 2, ctrl, 8);
            } else {
                set_imm(out, 1, ctrl, 8);
                set_reg(out, 2, reg_idx(modrm & 7, rex_b), 8);
            }
            return HB_OK;
        }
        if (op2 == 0xA0 || op2 == 0xA8) {
            out->opcode = HB_INS_PUSH_SEG;
            out->stack_delta = -8;
            set_imm(out, 1, op2 == 0xA0 ? 4 : 5, 2);
            return HB_OK;
        }
        if (op2 == 0xA1 || op2 == 0xA9) {
            out->opcode = HB_INS_POP_SEG;
            out->stack_delta = 8;
            set_imm(out, 1, op2 == 0xA1 ? 4 : 5, 2);
            return HB_OK;
        }
        if (op2 == 0x38) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            if (prefix_f2 && (op3 == 0xf0 || op3 == 0xf1)) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = HB_INS_CRC32;
                out->writes_flags = false;
                uint8_t dst_size = rex_w ? 8 : 4;
                uint8_t src_size = (op3 == 0xf0) ? 1 : (rex_w ? 8 : (operand16 ? 2 : 4));
                hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                            op3 == 0xf0 ? 4 : src_size, out, 1, 2, false);
                if (r != HB_OK) return r;
                out->op1.size = dst_size;
                if (op3 == 0xf0) {
                    if (out->op2.is_reg) {
                        uint8_t ro = 0;
                        out->op2.reg = reg8_idx(modrm & 7, out->has_rex, rex_b, &ro);
                        out->op2.reg_offset = ro;
                    }
                    out->op2.size = 1;
                } else {
                    out->op2.size = src_size;
                }
                return HB_OK;
            }
            if ((operand16 || prefix_f3) && op3 == 0xf6) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = prefix_f3 ? HB_INS_ADOX : HB_INS_ADCX;
                out->writes_flags = true;
                hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
                if (r != HB_OK) return r;
                out->op1.size = rex_w ? 8 : 4;
                out->op2.size = rex_w ? 8 : 4;
                return HB_OK;
            }
            bool movbe_redundant_f3_rex = prefix_f3 && has_rex && !prefix_f2;
            if (((!prefix_f2 && !prefix_f3) || movbe_redundant_f3_rex) &&
                (op3 == 0xf0 || op3 == 0xf1)) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                uint8_t size = movbe_redundant_f3_rex ? (operand16 ? 2 : 4) :
                               (rex_w ? 8 : (operand16 ? 2 : 4));
                out->opcode = HB_INS_MOVBE;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                            size, out, 1, 2, op3 == 0xf1);
                if (r != HB_OK) return r;
                out->op1.size = size;
                out->op2.size = size;
                return HB_OK;
            }
            if (op3 == 0xf9) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                uint8_t size = rex_w ? 8 : 4;
                out->opcode = HB_INS_MOVDIRI;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                            size, out, 1, 2, true);
                if (r != HB_OK) return r;
                out->op1.size = size;
                out->op2.size = size;
                return HB_OK;
            }
            if (operand16 && op3 == 0xf8) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                out->opcode = HB_INS_MOVDIR64B;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, true, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
                if (r != HB_OK) return r;
                out->op1.size = 8;
                if (out->op2.is_mem) out->op2.size = 64;
                return HB_OK;
            }
            if (op3 >= 0xc8 && op3 <= 0xcd) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                switch (op3) {
                    case 0xc8: out->opcode = HB_INS_SHA1NEXTE; break;
                    case 0xc9: out->opcode = HB_INS_SHA1MSG1; break;
                    case 0xca: out->opcode = HB_INS_SHA1MSG2; break;
                    case 0xcb: out->opcode = HB_INS_SHA256RNDS2; break;
                    case 0xcc: out->opcode = HB_INS_SHA256MSG1; break;
                    case 0xcd: out->opcode = HB_INS_SHA256MSG2; break;
                    default: return HB_ERR_UNSUPPORTED_OPCODE;
                }
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                if (out->op2.is_mem) out->op2.size = 16;
                return HB_OK;
            }
            if (operand16 && op3 == 0x2a) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                if ((modrm >> 6) == 3) return HB_ERR_UNSUPPORTED_OPCODE;
                out->opcode = HB_INS_MOVNTDQA;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                if (out->op2.is_mem) out->op2.size = 16;
                return HB_OK;
            }
            bool redundant_rex_mmx = (prefix_f2 || prefix_f3) && has_rex && !operand16 && !(prefix_f2 && prefix_f3);
            int vec_opcode = (operand16 || (!prefix_f2 && !prefix_f3) || redundant_rex_mmx) ?
                             ssse3_0f38_opcode(op3) : 0;
            if (!vec_opcode && operand16) vec_opcode = sse41_0f38_opcode(op3);
            if (vec_opcode) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                uint8_t modrm = read_u8(d);
                out->opcode = vec_opcode;
                out->writes_flags = false;
                uint8_t vec_bytes = operand16 ? 16 : 8;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, vec_bytes, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_vec_operands(out, vec_bytes);
                return HB_OK;
            }
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_VEC;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (op2 == 0x3A) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            uint8_t op3 = read_u8(d);
            uint8_t modrm = read_u8(d);
            if (op3 == 0xcc) {
                out->opcode = HB_INS_SHA1RNDS4;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                if (out->op2.is_mem) out->op2.size = 16;
                return HB_OK;
            }
            if (operand16 && op3 >= 0x08 && op3 <= 0x0b) {
                bool scalar = op3 == 0x0a || op3 == 0x0b;
                size_t mem_size = (op3 == 0x09 || op3 == 0x0b) ? 8 : (scalar ? 4 : 16);
                switch (op3) {
                    case 0x08: out->opcode = HB_INS_ROUNDPS; break;
                    case 0x09: out->opcode = HB_INS_ROUNDPD; break;
                    case 0x0a: out->opcode = HB_INS_ROUNDSS; break;
                    case 0x0b: out->opcode = HB_INS_ROUNDSD; break;
                    default: return HB_ERR_UNSUPPORTED_OPCODE;
                }
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                if (out->op2.is_mem) out->op2.size = mem_size;
                return HB_OK;
            }
            if (operand16 && (op3 == 0x40 || op3 == 0x41)) {
                size_t mem_size = 16;
                out->opcode = op3 == 0x41 ? HB_INS_DPPD : HB_INS_DPPS;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                if (out->op2.is_mem) out->op2.size = mem_size;
                return HB_OK;
            }
            if (operand16 && (op3 == 0x17 || op3 == 0x21)) {
                bool extract = op3 == 0x17;
                out->opcode = extract ? HB_INS_EXTRACTPS : HB_INS_INSERTPS;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, extract);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                if (extract) {
                    mark_xmm_operand(out, 2);
                    if (out->op1.is_mem) out->op1.size = 4;
                } else {
                    mark_xmm_operand(out, 1);
                    if (out->op2.is_reg) mark_xmm_operand(out, 2);
                    else if (out->op2.is_mem) out->op2.size = 4;
                }
                return HB_OK;
            }
            if (operand16 && (op3 == 0x14 || op3 == 0x15 || op3 == 0x16 ||
                              op3 == 0x20 || op3 == 0x22)) {
                bool extract = op3 == 0x14 || op3 == 0x15 || op3 == 0x16;
                uint8_t elem_size = 4;
                if (op3 == 0x14 || op3 == 0x20) elem_size = 1;
                else if (op3 == 0x15) elem_size = 2;
                else if (rex_w) elem_size = 8;
                if (extract) {
                    if (op3 == 0x14) out->opcode = HB_INS_PEXTRB;
                    else if (op3 == 0x15) out->opcode = HB_INS_PEXTRW;
                    else out->opcode = rex_w ? HB_INS_PEXTRQ : HB_INS_PEXTRD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                elem_size == 8 ? 8 : 4, out, 1, 2, true);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                    mark_xmm_operand(out, 2);
                    if (out->op1.is_mem) out->op1.size = elem_size;
                } else {
                    if (op3 == 0x20) out->opcode = HB_INS_PINSRB;
                    else out->opcode = rex_w ? HB_INS_PINSRQ : HB_INS_PINSRD;
                    hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                                elem_size == 8 ? 8 : 4, out, 1, 2, false);
                    if (r != HB_OK) return r;
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    set_extra_imm8(out, read_u8(d));
                    mark_xmm_operand(out, 1);
                    if (out->op2.is_mem) out->op2.size = elem_size;
                }
                return HB_OK;
            }
            if (operand16 && (op3 == 0xce || op3 == 0xcf)) {
                out->opcode = op3 == 0xce ? HB_INS_GF2P8AFFINEQB : HB_INS_GF2P8AFFINEINVQB;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_extra_imm8(out, read_u8(d));
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                if (out->op2.is_mem) out->op2.size = 16;
                return HB_OK;
            }
            bool palignr_redundant_rex_mmx = (prefix_f2 || prefix_f3) && has_rex && !operand16 &&
                                             !(prefix_f2 && prefix_f3) && op3 == 0x0f;
            int vec_opcode = operand16 ? sse41_0f3a_opcode(op3) :
                             (((!prefix_f2 && !prefix_f3 && op3 == 0x0f) ||
                               palignr_redundant_rex_mmx) ? HB_INS_PALIGNR : 0);
            out->opcode = vec_opcode ? vec_opcode : HB_INS_VEC;
            uint8_t vec_bytes = operand16 ? 16 : 8;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                        vec_opcode ? vec_bytes : 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 3, read_u8(d), 1);
            mark_vec_operands(out, vec_opcode ? vec_bytes : 16);
            return HB_OK;
        }
        /* Jcc near */
        if (op2 >= 0x80 && op2 <= 0x8F) {
            out->opcode = HB_INS_Jcc;
            out->is_branch = true;
            out->is_conditional = true;
            out->cond = cond_from_cc(op2 & 0x0F);
            int32_t rel;
            bool capstone_rel16 = operand16 && op2 <= 0x81;
            if (capstone_rel16) {
                if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
                rel = read_s16(d);
            } else {
                if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
                rel = read_s32(d);
            }
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
        if (op2 == 0xAE) {
            /* Control-state group needed by compiler setjmp helpers:
             *   0F AE /2 LDMXCSR m32  -- no-op until MXCSR is modeled
             *   0F AE /3 STMXCSR m32  -- store architectural reset MXCSR
             *   0F AE E8/F0/F8        -- LFENCE/MFENCE/SFENCE ordering fences
             * FXSAVE/FXRSTOR/XSAVE/XRSTOR are not scalar control-word ops and
             * require a real extended-state image, so keep them unsupported. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t ext = (modrm >> 3) & 7;
            if (prefix_f3 && mod == 3 && ext <= 3) {
                out->opcode = HB_INS_SYS;
                return parse_modrm_ext(d, modrm, rex_w, rex_b, rex_w ? 8 : 4, out, 1);
            }
            if (mod == 3) {
                if ((modrm & 7) == 0 && (ext == 5 || ext == 6 || ext == 7)) {
                    out->opcode = HB_INS_FENCE;
                    set_imm(out, 1, ext == 5 ? HB_FENCE_ACQUIRE :
                                    ext == 6 ? HB_FENCE_FULL : HB_FENCE_RELEASE, 1);
                    return HB_OK;
                }
                return HB_ERR_UNSUPPORTED_OPCODE;
            }
            if (ext == 2) {
                out->opcode = HB_INS_NOP;
                hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 4, out, 1);
                if (r != HB_OK) return r;
                return HB_OK;
            }
            if (ext == 3) {
                out->opcode = HB_INS_MOV;
                hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 4, out, 1);
                if (r != HB_OK) return r;
                set_imm(out, 2, HB_X64_DEFAULT_MXCSR, 4);
                return HB_OK;
            }
            if (ext == 7) {
                out->opcode = HB_INS_NOP;
                hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 1, out, 1);
                if (r != HB_OK) return r;
                return HB_OK;
            }
            return HB_ERR_UNSUPPORTED_OPCODE;
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
        if ((prefix_f3 || prefix_f2 || operand16) && op2 == 0xE6) {
            /* Packed 0F E6 conversion family:
             *   F3 0F E6    CVTDQ2PD xmm, xmm/m64
             *   F2 0F E6    CVTPD2DQ xmm, xmm/m128
             *   66 0F E6    CVTTPD2DQ xmm, xmm/m128
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 16;
            if (prefix_f3) {
                out->opcode = HB_INS_CVTDQ2PD;
                mem_size = 8;
            } else if (prefix_f2) {
                out->opcode = HB_INS_CVTPD2DQ;
            } else {
                out->opcode = HB_INS_CVTTPD2DQ;
            }
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
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
        if (op2 == 0x51) {
            /* Square-root family:
             *   0F 51       SQRTPS xmm, xmm/m128
             *   66 0F 51    SQRTPD xmm, xmm/m128
             *   F3 0F 51    SQRTSS xmm, xmm/m32
             *   F2 0F 51    SQRTSD xmm, xmm/m64
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = 16;
            if (prefix_f2) { out->opcode = HB_INS_SQRTSD; mem_size = 8; }
            else if (prefix_f3) { out->opcode = HB_INS_SQRTSS; mem_size = 4; }
            else if (operand16) out->opcode = HB_INS_SQRTPD;
            else out->opcode = HB_INS_SQRTPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
            return HB_OK;
        }
        if ((op2 == 0x52 || op2 == 0x53) && !operand16 && !prefix_f2) {
            /* Reciprocal-estimate family:
             *   0F 52       RSQRTPS xmm, xmm/m128
             *   F3 0F 52    RSQRTSS xmm, xmm/m32
             *   0F 53       RCPPS xmm, xmm/m128
             *   F3 0F 53    RCPSS xmm, xmm/m32
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            size_t mem_size = prefix_f3 ? 4 : 16;
            if (op2 == 0x52) out->opcode = prefix_f3 ? HB_INS_RSQRTSS : HB_INS_RSQRTPS;
            else out->opcode = prefix_f3 ? HB_INS_RCPSS : HB_INS_RCPPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, mem_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = mem_size;
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
        if ((op2 == 0x59 || op2 == 0x5E) && !prefix_f2 && !prefix_f3) {
            /* Packed floating MUL/DIV:
             *   0F 59/5E       MULPS/DIVPS xmm, xmm/m128
             *   66 0F 59/5E    MULPD/DIVPD xmm, xmm/m128
             * Scalar F2/F3 forms are handled by the existing MULSD/DIVSD/MULSS/DIVSS paths.
             */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (operand16) out->opcode = (op2 == 0x59) ? HB_INS_MULPD : HB_INS_DIVPD;
            else out->opcode = (op2 == 0x59) ? HB_INS_MULPS : HB_INS_DIVPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 16;
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
        if (prefix_f2 && op2 == 0x2D) {
            /* CVTSD2SI r32/r64, xmm/m64 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTSD2SI;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, false);
            if (r != HB_OK) return r;
            out->op1.size = rex_w ? 8 : 4;
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (prefix_f3 && op2 == 0x2D) {
            /* CVTSS2SI r32/r64, xmm/m32 */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_CVTSS2SI;
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
        if (operand16 && (op2 == 0x63 || op2 == 0x67 || op2 == 0x6B)) {
            /* SSE2 PACK signed/unsigned saturation family, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0x63) out->opcode = HB_INS_PACKSSWB;
            else if (op2 == 0x67) out->opcode = HB_INS_PACKUSWB;
            else out->opcode = HB_INS_PACKSSDW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
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
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && (op2 == 0xEC || op2 == 0xED || op2 == 0xDC || op2 == 0xDD)) {
            /* SSE2 packed saturating add family, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (op2 == 0xEC) out->opcode = HB_INS_PADDSB;
            else if (op2 == 0xED) out->opcode = HB_INS_PADDSW;
            else if (op2 == 0xDC) out->opcode = HB_INS_PADDUSB;
            else out->opcode = HB_INS_PADDUSW;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            return HB_OK;
        }
        if (operand16 && (op2 == 0xE0 || op2 == 0xE3)) {
            /* SSE2 packed rounded unsigned average family, xmm, xmm/m128. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = op2 == 0xE0 ? HB_INS_PAVGB : HB_INS_PAVGW;
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
        if (op2 == 0xC6 && !prefix_f2 && !prefix_f3) {
            /* SHUFPS/SHUFPD xmm, xmm/m128, imm8. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = operand16 ? HB_INS_SHUFPD : HB_INS_SHUFPS;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, 1);
            mark_xmm_operand(out, 2);
            if (out->op2.is_mem) out->op2.size = 16;
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
        if (!operand16 && (op2 == 0x71 || op2 == 0x72 || op2 == 0x73)) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            uint8_t ext = (modrm >> 3) & 7;
            if (!((op2 == 0x71 && (ext == 2 || ext == 4 || ext == 6)) ||
                  (op2 == 0x72 && (ext == 2 || ext == 4 || ext == 6)) ||
                  (op2 == 0x73 && (ext == 2 || ext == 3 || ext == 6 || ext == 7))))
                return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = HB_INS_MMX;
            out->writes_flags = false;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 8, out, 1);
            if (r != HB_OK) return r;
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, read_u8(d), 1);
            return HB_OK;
        }
        if (op2 == 0xB2 || op2 == 0xB4 || op2 == 0xB5) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_SYS;
            out->writes_flags = false;
            return parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
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
        if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {
            /* SHLD/SHRD r/m16/32/64, r16/32/64, imm8/CL. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = (op2 == 0xA4 || op2 == 0xA5) ? HB_INS_SHLD : HB_INS_SHRD;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                        rex_w ? 8 : (operand16 ? 2 : 4), out, 1, 2, true);
            if (r != HB_OK) return r;
            if (op2 == 0xA4 || op2 == 0xAC) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
            } else {
                set_reg(out, 3, HB_REG_RCX, 1);
            }
            return HB_OK;
        }
        if (op2 == 0xBC) {
            /* BSF or F3-prefixed TZCNT r32/64, r/m32/64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = prefix_f3 ? HB_INS_TZCNT : HB_INS_BSF;
            out->writes_flags = true;
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, 4, out, 1, 2, false);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0xBD) {
            /* BSR or F3-prefixed LZCNT r32/64, r/m32/64. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = prefix_f3 ? HB_INS_LZCNT : HB_INS_BSR;
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
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 16, out, 1, 2, false);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, 1);
                mark_xmm_operand(out, 2);
                return HB_OK;
            }
            if (op2 == 0x16 || op2 == 0x17) {
                out->opcode = operand16 ? HB_INS_MOVHPD : HB_INS_MOVHPS;
                out->writes_flags = false;
                hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, op2 == 0x17);
                if (r != HB_OK) return r;
                mark_xmm_operand(out, op2 == 0x17 ? 2 : 1);
                if (op2 == 0x17) out->op1.size = 8;
                else out->op2.size = 8;
                return HB_OK;
            }
            out->opcode = HB_INS_SSE_MOV;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b, 8, out, 1, 2, op2 == 0x13);
            if (r != HB_OK) return r;
            mark_xmm_operand(out, op2 == 0x13 ? 2 : 1);
            if (op2 == 0x13) out->op2.size = 8;
            else out->op1.size = 8;
            return HB_OK;
        }
        if (!operand16 && !prefix_f2 && !prefix_f3 && (op2 == 0x6F || op2 == 0x7F)) {
            /* Unprefixed 0F 6F/7F are MMX MOVQ. The 66-prefixed forms below
             * stay in the SSE_MOV path for MOVDQA/MOVDQU. */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_MMX;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                        8, out, 1, 2, op2 == 0x7F);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 ||
            op2 == 0x6F || op2 == 0x7F) {
            /* MOVUPS/MOVAPS/MOVDQA/MOVDQU are 128-bit moves.  Legacy scalar
             * MOVSS/MOVSD (F3/F2 0F 10/11) only transfers the low 4/8 bytes.
             * Register sources preserve the upper destination lanes; memory
             * sources zero them. */
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
        if (op2 == 0xC7) {
            /* CMPXCHG8B m64 / CMPXCHG16B m128 (REX.W). */
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (((modrm >> 3) & 7) != 1 || (modrm >> 6) == 3)
                return HB_ERR_UNSUPPORTED_OPCODE;
            out->opcode = HB_INS_CMPXCHG8B;
            out->writes_flags = true;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, rex_w ? 16 : 8, out, 1);
            if (r != HB_OK) return r;
            refresh_rip_targets(d, out);
            return HB_OK;
        }
        bool generic_0f_sys = false;
        bool generic_0f_vec = false;
        bool generic_0f_mmx = false;
        bool generic_0f_imm8 = false;
        bool generic_0f_imm8_second = false;
        if (!operand16 && !prefix_f2 && !prefix_f3 && (op2 == 0x78 || op2 == 0x79)) {
            generic_0f_sys = true; /* VMREAD/VMWRITE */
        } else if (prefix_f3 && op2 == 0xB8) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = HB_INS_POPCNT;
            out->writes_flags = true;
            uint8_t popcnt_size = rex_w ? 8 : (operand16 ? 2 : 4);
            hb_result_t r = parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b,
                                        popcnt_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            refresh_rip_targets(d, out);
            return HB_OK;
        } else if (prefix_f3 && op2 == 0xAE) {
            generic_0f_sys = true; /* RDFSBASE family */
        } else if ((operand16 || prefix_f2) && op2 == 0x78) {
            generic_0f_vec = true; /* EXTRQ/INSERTQ imm8, imm8 */
            generic_0f_imm8 = true;
            generic_0f_imm8_second = true;
        } else if ((operand16 || prefix_f2) && op2 == 0x79) {
            generic_0f_vec = true; /* EXTRQ/INSERTQ register form */
        } else if (operand16 && (op2 == 0x2A || op2 == 0x2C || op2 == 0x2D ||
                                 op2 == 0x7C || op2 == 0x7D || op2 == 0xD0 ||
                                 op2 == 0xE6 || op2 == 0xF7)) {
            generic_0f_vec = true;
        } else if (prefix_f2 && (op2 == 0x12 || op2 == 0x2D || op2 == 0x7C ||
                                 op2 == 0x7D || op2 == 0xD0 || op2 == 0xD6 ||
                                 op2 == 0xE6)) {
            generic_0f_vec = true;
        } else if (prefix_f3 && (op2 == 0x12 || op2 == 0x16 || op2 == 0x2D ||
                                 op2 == 0xD6)) {
            generic_0f_vec = true;
        }
        if (op2 == 0xC2) {
            /* 0F C2 /r ib = CMP{PS,PD,SS,SD} (imm8 predicate). The prefix selects
             * the variant: none=CMPPS (packed single), 66=CMPPD, F3=CMPSS, F2=CMPSD.
             * MacRunner: the no-prefix CMPPS case was missing from the gate (only
             * 66/F2/F3 were handled) -> no-prefix 0F C2 fell through to
             * HB_ERR_UNSUPPORTED_OPCODE. Unity 6 math/culling uses CMPLTPS
             * (predicate 1) at UnityPlayer rva 0x633f60. The imm8 predicate is read
             * uniformly below, so all 8 predicates (EQ/LT/LE/UNORD/NEQ/NLT/NLE/ORD)
             * are covered. */
            generic_0f_vec = true;
            generic_0f_imm8 = true;
        }
        int packed_0f = operand16 ? sse2_0f_packed_opcode(op2) : 0;
        if (packed_0f) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            out->opcode = packed_0f;
            out->writes_flags = false;
            uint8_t operand_size = (op2 == 0xC4 || op2 == 0xC5) ? 4 : 16;
            hb_result_t r = parse_modrm(d, modrm, false, rex_r, rex_x, rex_b,
                                        operand_size, out, 1, 2, false);
            if (r != HB_OK) return r;
            if (op2 == 0xC4) mark_xmm_operand(out, 1);
            else if (op2 == 0xC5) mark_xmm_operand(out, 2);
            else mark_vec_operands(out, 16);
            if (op2 == 0xC4 && out->op2.is_mem) out->op2.size = 2;
            if (op2 == 0xC4 || op2 == 0xC5) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
                refresh_rip_targets(d, out);
            }
            return HB_OK;
        }
        if (operand16 && (op2 == 0xC4 || op2 == 0xC5)) {
            generic_0f_mmx = true;
            generic_0f_imm8 = true;
        }
        if (operand16 && (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
                          op2 == 0xD8 || op2 == 0xD9 || op2 == 0xDA ||
                          op2 == 0xDE || op2 == 0xE1 || op2 == 0xE2 ||
                          op2 == 0xE8 || op2 == 0xE9 || op2 == 0xEA ||
                          op2 == 0xEE || op2 == 0xF1 || op2 == 0xF2 ||
                          op2 == 0xF3 || op2 == 0xF4 || op2 == 0xF6)) {
            generic_0f_mmx = true;
        }
        if (generic_0f_sys || generic_0f_vec || generic_0f_mmx) {
            if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
            uint8_t modrm = read_u8(d);
            if (generic_0f_sys) out->opcode = HB_INS_SYS;
            else if (generic_0f_mmx) out->opcode = HB_INS_MMX;
            else if (op2 == 0xC2) {
                /* 0F C2 = CMP{PS,PD,SS,SD} — the lift resolves the vec op via
                 * vec_op_from_ins(dec->opcode), so emit the SPECIFIC opcode
                 * (generic HB_INS_VEC would not resolve to a vop). Prefix selects
                 * the variant; imm8 predicate read below covers all 8 predicates. */
                if (operand16) out->opcode = HB_INS_VCMPPD;
                else if (prefix_f3) out->opcode = HB_INS_VCMPSS;
                else if (prefix_f2) out->opcode = HB_INS_VCMPSD;
                else out->opcode = HB_INS_VCMPPS;
            }
            else out->opcode = HB_INS_VEC;
            out->writes_flags = false;
            hb_result_t r = parse_modrm(d, modrm, generic_0f_sys && rex_w,
                                        rex_r, rex_x, rex_b,
                                        generic_0f_sys ? 4 : (generic_0f_mmx ? 8 : 16),
                                        out, 1, 2, false);
            if (r != HB_OK) return r;
            if (generic_0f_vec) mark_xmm_operands(out);
            if (generic_0f_imm8) {
                if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                set_imm(out, 3, read_u8(d), 1);
                if (generic_0f_imm8_second) {
                    if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
                    (void)read_u8(d);
                }
                refresh_rip_targets(d, out);
            }
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode >= 0xD8 && opcode <= 0xDF) {
        return decode_x87_x64(d, opcode, rex_b, out);
    }

    if (opcode == 0xD9) {
        /* x87 control-word scalar load/store pair used adjacent to STMXCSR in
           Wine's x64 setjmp helper. HyperBridge currently does not execute x87
           arithmetic, so FLDCW is a no-op and FNSTCW stores the reset control
           word expected by code saving a fresh thread context. */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t ext = (modrm >> 3) & 7;
        if (mod == 3) return HB_ERR_UNSUPPORTED_OPCODE;
        if (ext == 5) {
            out->opcode = HB_INS_NOP;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 2, out, 1);
            if (r != HB_OK) return r;
            return HB_OK;
        }
        if (ext == 7) {
            out->opcode = HB_INS_MOV;
            hb_result_t r = parse_modrm_ext(d, modrm, false, rex_b, 2, out, 1);
            if (r != HB_OK) return r;
            set_imm(out, 2, HB_X87_DEFAULT_CONTROL_WORD, 2);
            return HB_OK;
        }
        return HB_ERR_UNSUPPORTED_OPCODE;
    }

    if (opcode == 0xDB) {
        /* x87 environment control.  x64 HyperBridge does not model x87 state,
           so clearing/resetting pending x87 exception state is a no-op. */
        if (!can_read(d, 1)) return HB_ERR_DECODE_FAILED;
        uint8_t modrm = read_u8(d);
        if (modrm == 0xE2) {
            out->opcode = HB_INS_X87_FNCLEX;
            return HB_OK;
        }
        if (modrm == 0xE3) {
            out->opcode = HB_INS_X87_FNINIT;
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL;
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL;
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
        else if (ext == 2) out->opcode = HB_INS_RCL;
        else if (ext == 3) out->opcode = HB_INS_RCR;
        else if (ext == 4) out->opcode = HB_INS_SHL;
        else if (ext == 5) out->opcode = HB_INS_SHR;
        else if (ext == 6) out->opcode = HB_INS_SHL;
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
        if (ext == 0 || ext == 1) {
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
        if (ext == 0 || ext == 1) {
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
        if (ext == 3) {
            out->opcode = HB_INS_SYS;
            out->is_call = true;
            out->stack_delta = -16;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 16, out, 1);
        }
        if (ext == 4) {
            /* JMP r/m */
            out->opcode = HB_INS_JMP;
            out->is_branch = true;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 8, out, 1);
        }
        if (ext == 5) {
            out->opcode = HB_INS_SYS;
            out->is_branch = true;
            return parse_modrm_ext(d, modrm, rex_w, rex_b, 16, out, 1);
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
        return parse_modrm(d, modrm, rex_w, rex_r, rex_x, rex_b, op_size, out, 1, 2, true);
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
        uint8_t sz = rex_w ? 8 : (operand16 ? 2 : 4);
        set_reg(out, 1, HB_REG_RAX, sz);
        if (sz == 2) {
            if (!can_read(d, 2)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s16(d), sz);
        } else {
            if (!can_read(d, 4)) return HB_ERR_DECODE_FAILED;
            set_imm(out, 2, (int64_t)read_s32(d), sz);
        }
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
    hb_result_t r;
    if (d->arch == HB_ARCH_X86) {
        r = hb_decode_x86(d->code + d->pos, d->code_len - d->pos,
                          d->base_addr + d->pos, out);
    } else {
        r = hb_decode_x64(d->code + d->pos, d->code_len - d->pos,
                          d->base_addr + d->pos, out);
    }
    if (r == HB_OK || r == HB_ERR_UNSUPPORTED_OPCODE) {
        d->pos += out->len;
    } else if (r == HB_ERR_DECODE_FAILED) {
        d->pos += out->len > 0 ? out->len : 1;
    }
    return r;
}

hb_result_t hb_decode_at(hb_decoder_t* d, size_t offset, hb_decoded_t* out) {
    if (!d || !out || offset >= d->code_len) return HB_ERR_INVALID_ARG;
    if (d->arch == HB_ARCH_X86) {
        return hb_decode_x86(d->code + offset, d->code_len - offset,
                             d->base_addr + offset, out);
    }
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
        case HB_INS_MOV_SEG: return "MOV_SEG";
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
        case HB_INS_RCL: return "RCL";
        case HB_INS_RCR: return "RCR";
        case HB_INS_SHLD: return "SHLD";
        case HB_INS_SHRD: return "SHRD";
        case HB_INS_CMP: return "CMP";
        case HB_INS_TEST: return "TEST";
        case HB_INS_CMPXCHG: return "CMPXCHG";
        case HB_INS_CMPXCHG8B: return "CMPXCHG8B";
        case HB_INS_XCHG: return "XCHG";
        case HB_INS_XADD: return "XADD";
        case HB_INS_PUSH: return "PUSH";
        case HB_INS_POP: return "POP";
        case HB_INS_PUSHF: return "PUSHF";
        case HB_INS_POPF: return "POPF";
        case HB_INS_PUSH_SEG: return "PUSH_SEG";
        case HB_INS_POP_SEG: return "POP_SEG";
        case HB_INS_CALL: return "CALL";
        case HB_INS_RET: return "RET";
        case HB_INS_RETF: return "RETF";
        case HB_INS_ENTER: return "ENTER";
        case HB_INS_INT: return "INT";
        case HB_INS_INT1: return "INT1";
        case HB_INS_INT3: return "INT3";
        case HB_INS_INTO: return "INTO";
        case HB_INS_IRET: return "IRET";
        case HB_INS_JMP: return "JMP";
        case HB_INS_Jcc: return "Jcc";
        case HB_INS_LOOP: return "LOOP";
        case HB_INS_JRCXZ: return "JRCXZ";
        case HB_INS_SETcc: return "SETcc";
        case HB_INS_CMOVcc: return "CMOVcc";
        case HB_INS_MOVZX: return "MOVZX";
        case HB_INS_MOVSX: return "MOVSX";
        case HB_INS_MOVSXD: return "MOVSXD";
        case HB_INS_CDQE: return "CDQE";
        case HB_INS_CWDE: return "CWDE";
        case HB_INS_CWD: return "CWD";
        case HB_INS_LEAVE: return "LEAVE";
        case HB_INS_LAHF: return "LAHF";
        case HB_INS_SAHF: return "SAHF";
        case HB_INS_CPUID: return "CPUID";
        case HB_INS_XGETBV: return "XGETBV";
        case HB_INS_NOP: return "NOP";
        case HB_INS_FENCE: return "FENCE";
        case HB_INS_MOVS: return "MOVS";
        case HB_INS_CMPS: return "CMPS";
        case HB_INS_LODS: return "LODS";
        case HB_INS_SCAS: return "SCAS";
        case HB_INS_STOS: return "STOS";
        case HB_INS_INS: return "INS";
        case HB_INS_OUTS: return "OUTS";
        case HB_INS_IN: return "IN";
        case HB_INS_OUT: return "OUT";
        case HB_INS_XLAT: return "XLAT";
        case HB_INS_CLC: return "CLC";
        case HB_INS_STC: return "STC";
        case HB_INS_CMC: return "CMC";
        case HB_INS_CLD: return "CLD";
        case HB_INS_STD: return "STD";
        case HB_INS_CLI: return "CLI";
        case HB_INS_STI: return "STI";
        case HB_INS_HLT: return "HLT";
        case HB_INS_SALC: return "SALC";
        case HB_INS_MOV_CR: return "MOV_CR";
        case HB_INS_MOV_DR: return "MOV_DR";
        case HB_INS_BSF: return "BSF";
        case HB_INS_TZCNT: return "TZCNT";
        case HB_INS_LZCNT: return "LZCNT";
        case HB_INS_BSR: return "BSR";
        case HB_INS_BSWAP: return "BSWAP";
        case HB_INS_MOVBE: return "MOVBE";
        case HB_INS_MOVDIRI: return "MOVDIRI";
        case HB_INS_MOVDIR64B: return "MOVDIR64B";
        case HB_INS_CRC32: return "CRC32";
        case HB_INS_ANDN: return "ANDN";
        case HB_INS_BEXTR: return "BEXTR";
        case HB_INS_BLSI: return "BLSI";
        case HB_INS_BLSMSK: return "BLSMSK";
        case HB_INS_BLSR: return "BLSR";
        case HB_INS_BZHI: return "BZHI";
        case HB_INS_MULX: return "MULX";
        case HB_INS_PDEP: return "PDEP";
        case HB_INS_PEXT: return "PEXT";
        case HB_INS_RORX: return "RORX";
        case HB_INS_SARX: return "SARX";
        case HB_INS_SHLX: return "SHLX";
        case HB_INS_SHRX: return "SHRX";
        case HB_INS_ADCX: return "ADCX";
        case HB_INS_ADOX: return "ADOX";
        case HB_INS_SSE_MOV: return "SSE_MOV";
        case HB_INS_MOVNTDQA: return "MOVNTDQA";
        case HB_INS_MOVHLPS: return "MOVHLPS";
        case HB_INS_MOVLHPS: return "MOVLHPS";
        case HB_INS_VMOVHLPS: return "VMOVHLPS";
        case HB_INS_VMOVLHPS: return "VMOVLHPS";
        case HB_INS_VMOVLPS: return "VMOVLPS";
        case HB_INS_VMOVHPS: return "VMOVHPS";
        case HB_INS_VMOVLPD: return "VMOVLPD";
        case HB_INS_VMOVHPD: return "VMOVHPD";
        case HB_INS_MOVHPS: return "MOVHPS";
        case HB_INS_MOVHPD: return "MOVHPD";
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
        case HB_INS_PACKSSWB: return "PACKSSWB";
        case HB_INS_PACKUSWB: return "PACKUSWB";
        case HB_INS_PACKSSDW: return "PACKSSDW";
        case HB_INS_PMULLW: return "PMULLW";
        case HB_INS_PMULHW: return "PMULHW";
        case HB_INS_PMULHUW: return "PMULHUW";
        case HB_INS_PMADDWD: return "PMADDWD";
        case HB_INS_PADDSB: return "PADDSB";
        case HB_INS_PADDSW: return "PADDSW";
        case HB_INS_PADDUSB: return "PADDUSB";
        case HB_INS_PADDUSW: return "PADDUSW";
        case HB_INS_PAVGB: return "PAVGB";
        case HB_INS_PAVGW: return "PAVGW";
        case HB_INS_PSHUFB: return "PSHUFB";
        case HB_INS_PINSRW: return "PINSRW";
        case HB_INS_PEXTRW: return "PEXTRW";
        case HB_INS_PINSRB: return "PINSRB";
        case HB_INS_PINSRD: return "PINSRD";
        case HB_INS_PINSRQ: return "PINSRQ";
        case HB_INS_PEXTRB: return "PEXTRB";
        case HB_INS_PEXTRD: return "PEXTRD";
        case HB_INS_PEXTRQ: return "PEXTRQ";
        case HB_INS_INSERTPS: return "INSERTPS";
        case HB_INS_EXTRACTPS: return "EXTRACTPS";
        case HB_INS_PSHUFD: return "PSHUFD";
        case HB_INS_PSHUFLW: return "PSHUFLW";
        case HB_INS_PSHUFHW: return "PSHUFHW";
        case HB_INS_SHUFPS: return "SHUFPS";
        case HB_INS_SHUFPD: return "SHUFPD";
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
        case HB_INS_PHADDW: return "PHADDW";
        case HB_INS_PHADDD: return "PHADDD";
        case HB_INS_PHADDSW: return "PHADDSW";
        case HB_INS_PHSUBW: return "PHSUBW";
        case HB_INS_PHSUBD: return "PHSUBD";
        case HB_INS_PHSUBSW: return "PHSUBSW";
        case HB_INS_PMADDUBSW: return "PMADDUBSW";
        case HB_INS_PSIGNB: return "PSIGNB";
        case HB_INS_PSIGNW: return "PSIGNW";
        case HB_INS_PSIGND: return "PSIGND";
        case HB_INS_PMULHRSW: return "PMULHRSW";
        case HB_INS_PABSB: return "PABSB";
        case HB_INS_PABSW: return "PABSW";
        case HB_INS_PABSD: return "PABSD";
        case HB_INS_PTEST: return "PTEST";
        case HB_INS_VTESTPS: return "VTESTPS";
        case HB_INS_VTESTPD: return "VTESTPD";
        case HB_INS_PMOVSXBW: return "PMOVSXBW";
        case HB_INS_PMOVSXBD: return "PMOVSXBD";
        case HB_INS_PMOVSXBQ: return "PMOVSXBQ";
        case HB_INS_PMOVSXWD: return "PMOVSXWD";
        case HB_INS_PMOVSXWQ: return "PMOVSXWQ";
        case HB_INS_PMOVSXDQ: return "PMOVSXDQ";
        case HB_INS_PMULDQ: return "PMULDQ";
        case HB_INS_PCMPEQQ: return "PCMPEQQ";
        case HB_INS_PACKUSDW: return "PACKUSDW";
        case HB_INS_PMOVZXBW: return "PMOVZXBW";
        case HB_INS_PMOVZXBD: return "PMOVZXBD";
        case HB_INS_PMOVZXBQ: return "PMOVZXBQ";
        case HB_INS_PMOVZXWD: return "PMOVZXWD";
        case HB_INS_PMOVZXWQ: return "PMOVZXWQ";
        case HB_INS_PMOVZXDQ: return "PMOVZXDQ";
        case HB_INS_PCMPGTQ: return "PCMPGTQ";
        case HB_INS_PMINSB: return "PMINSB";
        case HB_INS_PMINSD: return "PMINSD";
        case HB_INS_PMINUW: return "PMINUW";
        case HB_INS_PMINUD: return "PMINUD";
        case HB_INS_PMAXSB: return "PMAXSB";
        case HB_INS_PMAXSD: return "PMAXSD";
        case HB_INS_PMAXUW: return "PMAXUW";
        case HB_INS_PMAXUD: return "PMAXUD";
        case HB_INS_PMULLD: return "PMULLD";
        case HB_INS_PHMINPOSUW: return "PHMINPOSUW";
        case HB_INS_MPSADBW: return "MPSADBW";
        case HB_INS_VMPSADBW: return "VMPSADBW";
        case HB_INS_PALIGNR: return "PALIGNR";
        case HB_INS_PBLENDW: return "PBLENDW";
        case HB_INS_BLENDPS: return "BLENDPS";
        case HB_INS_BLENDPD: return "BLENDPD";
        case HB_INS_PBLENDVB: return "PBLENDVB";
        case HB_INS_BLENDVPS: return "BLENDVPS";
        case HB_INS_BLENDVPD: return "BLENDVPD";
        case HB_INS_PSUBUSB: return "PSUBUSB";
        case HB_INS_PSUBUSW: return "PSUBUSW";
        case HB_INS_PSUBSB: return "PSUBSB";
        case HB_INS_PSUBSW: return "PSUBSW";
        case HB_INS_PMINUB: return "PMINUB";
        case HB_INS_PMINSW: return "PMINSW";
        case HB_INS_PMAXUB: return "PMAXUB";
        case HB_INS_PMAXSW: return "PMAXSW";
        case HB_INS_PMULUDQ: return "PMULUDQ";
        case HB_INS_PSADBW: return "PSADBW";
        case HB_INS_VZEROUPPER: return "VZEROUPPER";
        case HB_INS_VPBROADCASTB: return "VPBROADCASTB";
        case HB_INS_VPBROADCASTW: return "VPBROADCASTW";
        case HB_INS_VPBROADCASTD: return "VPBROADCASTD";
        case HB_INS_VPBROADCASTQ: return "VPBROADCASTQ";
        case HB_INS_VBROADCASTSS: return "VBROADCASTSS";
        case HB_INS_VBROADCASTSD: return "VBROADCASTSD";
        case HB_INS_VBROADCASTF32X2: return "VBROADCASTF32X2";
        case HB_INS_VBROADCASTF64X2: return "VBROADCASTF64X2";
        case HB_INS_VBROADCASTF32X4: return "VBROADCASTF32X4";
        case HB_INS_VBROADCASTF64X4: return "VBROADCASTF64X4";
        case HB_INS_VBROADCASTF32X8: return "VBROADCASTF32X8";
        case HB_INS_VBROADCASTI32X2: return "VBROADCASTI32X2";
        case HB_INS_VBROADCASTI128: return "VBROADCASTI128";
        case HB_INS_VPBLENDD: return "VPBLENDD";
        case HB_INS_VPERMQ: return "VPERMQ";
        case HB_INS_VPERMPD: return "VPERMPD";
        case HB_INS_VPERMILPS: return "VPERMILPS";
        case HB_INS_VPERMILPD: return "VPERMILPD";
        case HB_INS_VBLENDVPS: return "VBLENDVPS";
        case HB_INS_VBLENDVPD: return "VBLENDVPD";
        case HB_INS_VPBLENDVB: return "VPBLENDVB";
        case HB_INS_VINSERTF128: return "VINSERTF128";
        case HB_INS_VINSERTI128: return "VINSERTI128";
        case HB_INS_VEXTRACTF128: return "VEXTRACTF128";
        case HB_INS_VEXTRACTI128: return "VEXTRACTI128";
        case HB_INS_VPERM2F128: return "VPERM2F128";
        case HB_INS_VPERM2I128: return "VPERM2I128";
        case HB_INS_VPSRLVD: return "VPSRLVD";
        case HB_INS_VPSRLVQ: return "VPSRLVQ";
        case HB_INS_VPSRAVD: return "VPSRAVD";
        case HB_INS_VPSLLVD: return "VPSLLVD";
        case HB_INS_VPSLLVQ: return "VPSLLVQ";
        case HB_INS_PCLMULQDQ: return "PCLMULQDQ";
        case HB_INS_AESKEYGENASSIST: return "AESKEYGENASSIST";
        case HB_INS_VPCLMULQDQ: return "VPCLMULQDQ";
        case HB_INS_AESIMC: return "AESIMC";
        case HB_INS_AESENC: return "AESENC";
        case HB_INS_AESENCLAST: return "AESENCLAST";
        case HB_INS_AESDEC: return "AESDEC";
        case HB_INS_AESDECLAST: return "AESDECLAST";
        case HB_INS_VAESENC: return "VAESENC";
        case HB_INS_VAESENCLAST: return "VAESENCLAST";
        case HB_INS_VAESDEC: return "VAESDEC";
        case HB_INS_VAESDECLAST: return "VAESDECLAST";
        case HB_INS_GF2P8MULB: return "GF2P8MULB";
        case HB_INS_VGF2P8MULB: return "VGF2P8MULB";
        case HB_INS_GF2P8AFFINEQB: return "GF2P8AFFINEQB";
        case HB_INS_GF2P8AFFINEINVQB: return "GF2P8AFFINEINVQB";
        case HB_INS_VGF2P8AFFINEQB: return "VGF2P8AFFINEQB";
        case HB_INS_VGF2P8AFFINEINVQB: return "VGF2P8AFFINEINVQB";
        case HB_INS_SHA1NEXTE: return "SHA1NEXTE";
        case HB_INS_SHA1MSG1: return "SHA1MSG1";
        case HB_INS_SHA1MSG2: return "SHA1MSG2";
        case HB_INS_SHA256RNDS2: return "SHA256RNDS2";
        case HB_INS_SHA256MSG1: return "SHA256MSG1";
        case HB_INS_SHA256MSG2: return "SHA256MSG2";
        case HB_INS_SHA1RNDS4: return "SHA1RNDS4";
        case HB_INS_VPERMD: return "VPERMD";
        case HB_INS_VPERMPS: return "VPERMPS";
        case HB_INS_VMASKMOVPS: return "VMASKMOVPS";
        case HB_INS_VMASKMOVPD: return "VMASKMOVPD";
        case HB_INS_VMASKMOVDQU: return "VMASKMOVDQU";
        case HB_INS_VPMASKMOVD: return "VPMASKMOVD";
        case HB_INS_VPMASKMOVQ: return "VPMASKMOVQ";
        case HB_INS_VGATHERDPS: return "VGATHERDPS";
        case HB_INS_VGATHERDPD: return "VGATHERDPD";
        case HB_INS_VGATHERQPS: return "VGATHERQPS";
        case HB_INS_VGATHERQPD: return "VGATHERQPD";
        case HB_INS_VPGATHERDD: return "VPGATHERDD";
        case HB_INS_VPGATHERDQ: return "VPGATHERDQ";
        case HB_INS_VPGATHERQD: return "VPGATHERQD";
        case HB_INS_VPGATHERQQ: return "VPGATHERQQ";
        case HB_INS_PCMPESTRM: return "PCMPESTRM";
        case HB_INS_PCMPESTRI: return "PCMPESTRI";
        case HB_INS_PCMPISTRM: return "PCMPISTRM";
        case HB_INS_PCMPISTRI: return "PCMPISTRI";
        case HB_INS_MOVD: return "MOVD";
        case HB_INS_CVTDQ2PD: return "CVTDQ2PD";
        case HB_INS_CVTDQ2PS: return "CVTDQ2PS";
        case HB_INS_CVTPS2DQ: return "CVTPS2DQ";
        case HB_INS_CVTTPS2DQ: return "CVTTPS2DQ";
        case HB_INS_CVTPS2PD: return "CVTPS2PD";
        case HB_INS_CVTPD2PS: return "CVTPD2PS";
        case HB_INS_CVTPD2DQ: return "CVTPD2DQ";
        case HB_INS_CVTTPD2DQ: return "CVTTPD2DQ";
        case HB_INS_CVTSS2SD: return "CVTSS2SD";
        case HB_INS_CVTSD2SS: return "CVTSD2SS";
        case HB_INS_CVTSI2SD: return "CVTSI2SD";
        case HB_INS_CVTSI2SS: return "CVTSI2SS";
        case HB_INS_SQRTPS: return "SQRTPS";
        case HB_INS_SQRTPD: return "SQRTPD";
        case HB_INS_SQRTSS: return "SQRTSS";
        case HB_INS_SQRTSD: return "SQRTSD";
        case HB_INS_RSQRTPS: return "RSQRTPS";
        case HB_INS_RSQRTSS: return "RSQRTSS";
        case HB_INS_RCPPS: return "RCPPS";
        case HB_INS_RCPSS: return "RCPSS";
        case HB_INS_ROUNDPS: return "ROUNDPS";
        case HB_INS_ROUNDPD: return "ROUNDPD";
        case HB_INS_ROUNDSS: return "ROUNDSS";
        case HB_INS_ROUNDSD: return "ROUNDSD";
        case HB_INS_DPPS: return "DPPS";
        case HB_INS_DPPD: return "DPPD";
        case HB_INS_ADDPS: return "ADDPS";
        case HB_INS_ADDPD: return "ADDPD";
        case HB_INS_ADDSS: return "ADDSS";
        case HB_INS_ADDSD: return "ADDSD";
        case HB_INS_SUBPS: return "SUBPS";
        case HB_INS_SUBPD: return "SUBPD";
        case HB_INS_SUBSS: return "SUBSS";
        case HB_INS_SUBSD: return "SUBSD";
        case HB_INS_MULPS: return "MULPS";
        case HB_INS_MULPD: return "MULPD";
        case HB_INS_DIVPS: return "DIVPS";
        case HB_INS_DIVPD: return "DIVPD";
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
        case HB_INS_VFMADD132PS: return "VFMADD132PS";
        case HB_INS_VFMADD132PD: return "VFMADD132PD";
        case HB_INS_VFMADD132SS: return "VFMADD132SS";
        case HB_INS_VFMADD132SD: return "VFMADD132SD";
        case HB_INS_VFMADD213PS: return "VFMADD213PS";
        case HB_INS_VFMADD213PD: return "VFMADD213PD";
        case HB_INS_VFMADD213SS: return "VFMADD213SS";
        case HB_INS_VFMADD213SD: return "VFMADD213SD";
        case HB_INS_VFMADD231PS: return "VFMADD231PS";
        case HB_INS_VFMADD231PD: return "VFMADD231PD";
        case HB_INS_VFMADD231SS: return "VFMADD231SS";
        case HB_INS_VFMADD231SD: return "VFMADD231SD";
        case HB_INS_VFMSUB132PS: return "VFMSUB132PS";
        case HB_INS_VFMSUB132PD: return "VFMSUB132PD";
        case HB_INS_VFMSUB132SS: return "VFMSUB132SS";
        case HB_INS_VFMSUB132SD: return "VFMSUB132SD";
        case HB_INS_VFMSUB213PS: return "VFMSUB213PS";
        case HB_INS_VFMSUB213PD: return "VFMSUB213PD";
        case HB_INS_VFMSUB213SS: return "VFMSUB213SS";
        case HB_INS_VFMSUB213SD: return "VFMSUB213SD";
        case HB_INS_VFMSUB231PS: return "VFMSUB231PS";
        case HB_INS_VFMSUB231PD: return "VFMSUB231PD";
        case HB_INS_VFMSUB231SS: return "VFMSUB231SS";
        case HB_INS_VFMSUB231SD: return "VFMSUB231SD";
        case HB_INS_VFMADDSUB132PS: return "VFMADDSUB132PS";
        case HB_INS_VFMADDSUB132PD: return "VFMADDSUB132PD";
        case HB_INS_VFMSUBADD132PS: return "VFMSUBADD132PS";
        case HB_INS_VFMSUBADD132PD: return "VFMSUBADD132PD";
        case HB_INS_VFMADDSUB213PS: return "VFMADDSUB213PS";
        case HB_INS_VFMADDSUB213PD: return "VFMADDSUB213PD";
        case HB_INS_VFMSUBADD213PS: return "VFMSUBADD213PS";
        case HB_INS_VFMSUBADD213PD: return "VFMSUBADD213PD";
        case HB_INS_VFMADDSUB231PS: return "VFMADDSUB231PS";
        case HB_INS_VFMADDSUB231PD: return "VFMADDSUB231PD";
        case HB_INS_VFMSUBADD231PS: return "VFMSUBADD231PS";
        case HB_INS_VFMSUBADD231PD: return "VFMSUBADD231PD";
        case HB_INS_VFNMADD132PS: return "VFNMADD132PS";
        case HB_INS_VFNMADD132PD: return "VFNMADD132PD";
        case HB_INS_VFNMADD132SS: return "VFNMADD132SS";
        case HB_INS_VFNMADD132SD: return "VFNMADD132SD";
        case HB_INS_VFNMSUB132PS: return "VFNMSUB132PS";
        case HB_INS_VFNMSUB132PD: return "VFNMSUB132PD";
        case HB_INS_VFNMSUB132SS: return "VFNMSUB132SS";
        case HB_INS_VFNMSUB132SD: return "VFNMSUB132SD";
        case HB_INS_VFNMADD213PS: return "VFNMADD213PS";
        case HB_INS_VFNMADD213PD: return "VFNMADD213PD";
        case HB_INS_VFNMADD213SS: return "VFNMADD213SS";
        case HB_INS_VFNMADD213SD: return "VFNMADD213SD";
        case HB_INS_VFNMSUB213PS: return "VFNMSUB213PS";
        case HB_INS_VFNMSUB213PD: return "VFNMSUB213PD";
        case HB_INS_VFNMSUB213SS: return "VFNMSUB213SS";
        case HB_INS_VFNMSUB213SD: return "VFNMSUB213SD";
        case HB_INS_VFNMADD231PS: return "VFNMADD231PS";
        case HB_INS_VFNMADD231PD: return "VFNMADD231PD";
        case HB_INS_VFNMADD231SS: return "VFNMADD231SS";
        case HB_INS_VFNMADD231SD: return "VFNMADD231SD";
        case HB_INS_VFNMSUB231PS: return "VFNMSUB231PS";
        case HB_INS_VFNMSUB231PD: return "VFNMSUB231PD";
        case HB_INS_VFNMSUB231SS: return "VFNMSUB231SS";
        case HB_INS_VFNMSUB231SD: return "VFNMSUB231SD";
        case HB_INS_VCVTPH2PS: return "VCVTPH2PS";
        case HB_INS_VCVTPS2PH: return "VCVTPS2PH";
        case HB_INS_VCMPPS: return "VCMPPS";
        case HB_INS_VCMPPD: return "VCMPPD";
        case HB_INS_VCMPSS: return "VCMPSS";
        case HB_INS_VCMPSD: return "VCMPSD";
        case HB_INS_VHADDPS: return "VHADDPS";
        case HB_INS_VHADDPD: return "VHADDPD";
        case HB_INS_VHSUBPS: return "VHSUBPS";
        case HB_INS_VHSUBPD: return "VHSUBPD";
        case HB_INS_VADDSUBPS: return "VADDSUBPS";
        case HB_INS_VADDSUBPD: return "VADDSUBPD";
        case HB_INS_VMOVSLDUP: return "VMOVSLDUP";
        case HB_INS_VMOVSHDUP: return "VMOVSHDUP";
        case HB_INS_VMOVDDUP: return "VMOVDDUP";
        case HB_INS_COMISS: return "COMISS";
        case HB_INS_COMISD: return "COMISD";
        case HB_INS_CVTSD2SI: return "CVTSD2SI";
        case HB_INS_CVTSS2SI: return "CVTSS2SI";
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
        case HB_INS_X87_FLD: return "X87_FLD";
        case HB_INS_X87_FST: return "X87_FST";
        case HB_INS_X87_FSTP: return "X87_FSTP";
        case HB_INS_X87_FILD: return "X87_FILD";
        case HB_INS_X87_FISTP: return "X87_FISTP";
        case HB_INS_X87_FIST: return "X87_FIST";
        case HB_INS_X87_FISTTP: return "X87_FISTTP";
        case HB_INS_X87_FI: return "X87_FI";
        case HB_INS_X87_FLDCW: return "X87_FLDCW";
        case HB_INS_X87_FNSTCW: return "X87_FNSTCW";
        case HB_INS_X87_FNSTSW: return "X87_FNSTSW";
        case HB_INS_X87_FLDENV: return "X87_FLDENV";
        case HB_INS_X87_FNSTENV: return "X87_FNSTENV";
        case HB_INS_X87_FRSTOR: return "X87_FRSTOR";
        case HB_INS_X87_FNSAVE: return "X87_FNSAVE";
        case HB_INS_X87_FXSAVE: return "X87_FXSAVE";
        case HB_INS_X87_FXRSTOR: return "X87_FXRSTOR";
        case HB_INS_X87_FADD: return "X87_FADD";
        case HB_INS_X87_FMUL: return "X87_FMUL";
        case HB_INS_X87_FCOM: return "X87_FCOM";
        case HB_INS_X87_FCOMP: return "X87_FCOMP";
        case HB_INS_X87_FSUB: return "X87_FSUB";
        case HB_INS_X87_FSUBR: return "X87_FSUBR";
        case HB_INS_X87_FDIV: return "X87_FDIV";
        case HB_INS_X87_FDIVR: return "X87_FDIVR";
        case HB_INS_X87_FADDP: return "X87_FADDP";
        case HB_INS_X87_FMULP: return "X87_FMULP";
        case HB_INS_X87_FCOMPP: return "X87_FCOMPP";
        case HB_INS_X87_FSUBP: return "X87_FSUBP";
        case HB_INS_X87_FSUBRP: return "X87_FSUBRP";
        case HB_INS_X87_FDIVP: return "X87_FDIVP";
        case HB_INS_X87_FDIVRP: return "X87_FDIVRP";
        case HB_INS_X87_FXCH: return "X87_FXCH";
        case HB_INS_X87_FRNDINT: return "X87_FRNDINT";
        case HB_INS_X87_FINCSTP: return "X87_FINCSTP";
        case HB_INS_X87_FDECSTP: return "X87_FDECSTP";
        case HB_INS_X87_FNCLEX: return "X87_FNCLEX";
        case HB_INS_X87_FNINIT: return "X87_FNINIT";
        case HB_INS_X87_FXAM: return "X87_FXAM";
        case HB_INS_X87_FSQRT: return "X87_FSQRT";
        case HB_INS_X87_F2XM1: return "X87_F2XM1";
        case HB_INS_X87_FYL2X: return "X87_FYL2X";
        case HB_INS_X87_FPTAN: return "X87_FPTAN";
        case HB_INS_X87_FPATAN: return "X87_FPATAN";
        case HB_INS_X87_FXTRACT: return "X87_FXTRACT";
        case HB_INS_X87_FPREM1: return "X87_FPREM1";
        case HB_INS_X87_FPREM: return "X87_FPREM";
        case HB_INS_X87_FYL2XP1: return "X87_FYL2XP1";
        case HB_INS_X87_FSINCOS: return "X87_FSINCOS";
        case HB_INS_X87_FSCALE: return "X87_FSCALE";
        case HB_INS_X87_FSIN: return "X87_FSIN";
        case HB_INS_X87_FCOS: return "X87_FCOS";
        case HB_INS_X87_FCMOV: return "X87_FCMOV";
        case HB_INS_X87_FCOMI: return "X87_FCOMI";
        case HB_INS_X87_FUCOMI: return "X87_FUCOMI";
        case HB_INS_X87_FCOMPI: return "X87_FCOMPI";
        case HB_INS_X87_FUCOMPI: return "X87_FUCOMPI";
        case HB_INS_X87_FUCOM: return "X87_FUCOM";
        case HB_INS_X87_FUCOMP: return "X87_FUCOMP";
        case HB_INS_X87_FFREE: return "X87_FFREE";
        case HB_INS_X87_FFREEP: return "X87_FFREEP";
        case HB_INS_X87_FNOP: return "X87_FNOP";
        case HB_INS_X87_FCHS: return "X87_FCHS";
        case HB_INS_X87_FABS: return "X87_FABS";
        case HB_INS_X87_FTST: return "X87_FTST";
        case HB_INS_X87_MISC: return "X87_MISC";
        case HB_INS_PUSHA: return "PUSHA";
        case HB_INS_POPA: return "POPA";
        case HB_INS_AAA: return "AAA";
        case HB_INS_AAS: return "AAS";
        case HB_INS_AAM: return "AAM";
        case HB_INS_AAD: return "AAD";
        case HB_INS_DAA: return "DAA";
        case HB_INS_DAS: return "DAS";
        case HB_INS_BOUND: return "BOUND";
        case HB_INS_ARPL: return "ARPL";
        case HB_INS_LDS: return "LDS";
        case HB_INS_LES: return "LES";
        case HB_INS_LFS: return "LFS";
        case HB_INS_LGS: return "LGS";
        case HB_INS_SYS: return "SYS";
        case HB_INS_UD: return "UD";
        case HB_INS_MMX: return "MMX";
        case HB_INS_VEC: return "VEC";
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
