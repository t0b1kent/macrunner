#include "hb_lifter.h"
#include "hb_ir.h"
#include <string.h>

static hb_size_t size_from_dec(uint8_t sz) {
    switch (sz) {
        case 1: return HB_SIZE_8;
        case 2: return HB_SIZE_16;
        case 4: return HB_SIZE_32;
        case 8: return HB_SIZE_64;
        case 10: return HB_SIZE_80;
        case 16: return HB_SIZE_128;
        case 32: return HB_SIZE_256;
        case 64: return HB_SIZE_512;
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
        bool vsib = (slot == 1) ? d->op1.mem.vsib : (slot == 2) ? d->op2.mem.vsib : d->op3.mem.vsib;
        uint8_t vsib_index_size = (slot == 1) ? d->op1.mem.vsib_index_size : (slot == 2) ? d->op2.mem.vsib_index_size : d->op3.mem.vsib_index_size;
        uint8_t vsib_elem_size = (slot == 1) ? d->op1.mem.vsib_elem_size : (slot == 2) ? d->op2.mem.vsib_elem_size : d->op3.mem.vsib_elem_size;
        uint8_t vsib_count = (slot == 1) ? d->op1.mem.vsib_count : (slot == 2) ? d->op2.mem.vsib_count : d->op3.mem.vsib_count;
        if (rip_relative) {
            base = -1;
            disp += (int64_t)rip_target;
        }
        hb_ir_operand_t op = hb_ir_mem_segment(
            (hb_reg_t)(base >= 0 ? base : HB_REG_COUNT),
            (hb_reg_t)(index >= 0 ? index : HB_REG_COUNT),
            scale, disp, sz, segment);
        op.mem.addr32 = addr32;
        op.mem.vsib = vsib;
        op.mem.vsib_index_size = vsib_index_size;
        op.mem.vsib_elem_size = vsib_elem_size;
        op.mem.vsib_count = vsib_count;
        return op;
    }
    return hb_ir_none();
}

static bool has_vex_src(const hb_decoded_t* dec) {
    return dec->op3.present && !dec->op3.is_imm;
}

static bool is_vex_decoded(const hb_decoded_t* dec) {
    return dec && dec->len > 0 && (dec->evex || dec->bytes[0] == 0xc4 || dec->bytes[0] == 0xc5);
}

static bool is_evex_decoded(const hb_decoded_t* dec) {
    return dec && dec->evex;
}

static uint64_t evex_target_arg(const hb_decoded_t* dec, uint64_t arg) {
    if (!is_evex_decoded(dec)) return arg;
    return (arg & 0x00ffffffu) |
           ((uint64_t)(dec->evex_mask & 7u) << 24) |
           (dec->evex_zero ? (1ULL << 27) : 0) |
           (1ULL << 28);
}

static void set_evex_target_arg(hb_ir_instr_t* i, const hb_decoded_t* dec, uint64_t arg) {
    if (i && is_evex_decoded(dec)) i->target = evex_target_arg(dec, arg);
}

#define HB_EVEX_ARG_BROADCAST 0x200u
#define HB_EVEX_ARG_ROUND_SHIFT 10
#define HB_EVEX_ARG_ROUND_MASK  0x1c00u

static hb_ir_operand_t vector_src1_from_dec(const hb_decoded_t* dec, hb_ir_operand_t legacy_dst) {
    return has_vex_src(dec) ? operand_from_dec(dec, 2) : legacy_dst;
}

static hb_ir_operand_t vector_src2_from_dec(const hb_decoded_t* dec) {
    return has_vex_src(dec) ? operand_from_dec(dec, 3) : operand_from_dec(dec, 2);
}

static uint64_t vec_target(hb_ir_vec_op_t op, uint64_t arg) {
    return ((uint64_t)op << 32) | arg;
}

static uint32_t fma_target_from_ins(int opcode) {
    bool scalar = opcode == HB_INS_VFMADD132SS || opcode == HB_INS_VFMADD132SD ||
                  opcode == HB_INS_VFMADD213SS || opcode == HB_INS_VFMADD213SD ||
                  opcode == HB_INS_VFMADD231SS || opcode == HB_INS_VFMADD231SD ||
                  opcode == HB_INS_VFMSUB132SS || opcode == HB_INS_VFMSUB132SD ||
                  opcode == HB_INS_VFMSUB213SS || opcode == HB_INS_VFMSUB213SD ||
                  opcode == HB_INS_VFMSUB231SS || opcode == HB_INS_VFMSUB231SD ||
                  opcode == HB_INS_VFNMADD132SS || opcode == HB_INS_VFNMADD132SD ||
                  opcode == HB_INS_VFNMSUB132SS || opcode == HB_INS_VFNMSUB132SD ||
                  opcode == HB_INS_VFNMADD213SS || opcode == HB_INS_VFNMADD213SD ||
                  opcode == HB_INS_VFNMSUB213SS || opcode == HB_INS_VFNMSUB213SD ||
                  opcode == HB_INS_VFNMADD231SS || opcode == HB_INS_VFNMADD231SD ||
                  opcode == HB_INS_VFNMSUB231SS || opcode == HB_INS_VFNMSUB231SD;
    bool fp64 = opcode == HB_INS_VFMADD132PD || opcode == HB_INS_VFMADD132SD ||
                opcode == HB_INS_VFMADD213PD || opcode == HB_INS_VFMADD213SD ||
                opcode == HB_INS_VFMADD231PD || opcode == HB_INS_VFMADD231SD ||
                opcode == HB_INS_VFMSUB132PD || opcode == HB_INS_VFMSUB132SD ||
                opcode == HB_INS_VFMSUB213PD || opcode == HB_INS_VFMSUB213SD ||
                opcode == HB_INS_VFMSUB231PD || opcode == HB_INS_VFMSUB231SD ||
                opcode == HB_INS_VFMADDSUB132PD || opcode == HB_INS_VFMSUBADD132PD ||
                opcode == HB_INS_VFMADDSUB213PD || opcode == HB_INS_VFMSUBADD213PD ||
                opcode == HB_INS_VFMADDSUB231PD || opcode == HB_INS_VFMSUBADD231PD ||
                opcode == HB_INS_VFNMADD132PD || opcode == HB_INS_VFNMADD132SD ||
                opcode == HB_INS_VFNMSUB132PD || opcode == HB_INS_VFNMSUB132SD ||
                opcode == HB_INS_VFNMADD213PD || opcode == HB_INS_VFNMADD213SD ||
                opcode == HB_INS_VFNMSUB213PD || opcode == HB_INS_VFNMSUB213SD ||
                opcode == HB_INS_VFNMADD231PD || opcode == HB_INS_VFNMADD231SD ||
                opcode == HB_INS_VFNMSUB231PD || opcode == HB_INS_VFNMSUB231SD;
    return (fp64 ? 8u : 4u) | (scalar ? 0x100u : 0u);
}

static uint8_t dec_imm8(const hb_decoded_t* dec) {
    if (dec->has_imm8) return dec->imm8;
    if (dec->op3.present && dec->op3.is_imm) return (uint8_t)dec->op3.imm;
    return 0;
}

static hb_ir_vec_op_t vec_op_from_ins(int opcode) {
    switch (opcode) {
        case HB_INS_PHADDW: return HB_VEC_PHADDW;
        case HB_INS_PHADDD: return HB_VEC_PHADDD;
        case HB_INS_PHADDSW: return HB_VEC_PHADDSW;
        case HB_INS_PHSUBW: return HB_VEC_PHSUBW;
        case HB_INS_PHSUBD: return HB_VEC_PHSUBD;
        case HB_INS_PHSUBSW: return HB_VEC_PHSUBSW;
        case HB_INS_PMADDUBSW: return HB_VEC_PMADDUBSW;
        case HB_INS_PSIGNB: return HB_VEC_PSIGNB;
        case HB_INS_PSIGNW: return HB_VEC_PSIGNW;
        case HB_INS_PSIGND: return HB_VEC_PSIGND;
        case HB_INS_PMULHRSW: return HB_VEC_PMULHRSW;
        case HB_INS_PABSB: return HB_VEC_PABSB;
        case HB_INS_PABSW: return HB_VEC_PABSW;
        case HB_INS_PABSD: return HB_VEC_PABSD;
        case HB_INS_PTEST: return HB_VEC_PTEST;
        case HB_INS_VTESTPS: return HB_VEC_VTESTPS;
        case HB_INS_VTESTPD: return HB_VEC_VTESTPD;
        case HB_INS_PMOVSXBW: return HB_VEC_PMOVSXBW;
        case HB_INS_PMOVSXBD: return HB_VEC_PMOVSXBD;
        case HB_INS_PMOVSXBQ: return HB_VEC_PMOVSXBQ;
        case HB_INS_PMOVSXWD: return HB_VEC_PMOVSXWD;
        case HB_INS_PMOVSXWQ: return HB_VEC_PMOVSXWQ;
        case HB_INS_PMOVSXDQ: return HB_VEC_PMOVSXDQ;
        case HB_INS_PMULDQ: return HB_VEC_PMULDQ;
        case HB_INS_PCMPEQQ: return HB_VEC_PCMPEQQ;
        case HB_INS_PACKUSDW: return HB_VEC_PACKUSDW;
        case HB_INS_PMOVZXBW: return HB_VEC_PMOVZXBW;
        case HB_INS_PMOVZXBD: return HB_VEC_PMOVZXBD;
        case HB_INS_PMOVZXBQ: return HB_VEC_PMOVZXBQ;
        case HB_INS_PMOVZXWD: return HB_VEC_PMOVZXWD;
        case HB_INS_PMOVZXWQ: return HB_VEC_PMOVZXWQ;
        case HB_INS_PMOVZXDQ: return HB_VEC_PMOVZXDQ;
        case HB_INS_PCMPGTQ: return HB_VEC_PCMPGTQ;
        case HB_INS_PMINSB: return HB_VEC_PMINSB;
        case HB_INS_PMINSD: return HB_VEC_PMINSD;
        case HB_INS_PMINUW: return HB_VEC_PMINUW;
        case HB_INS_PMINUD: return HB_VEC_PMINUD;
        case HB_INS_PMAXSB: return HB_VEC_PMAXSB;
        case HB_INS_PMAXSD: return HB_VEC_PMAXSD;
        case HB_INS_PMAXUW: return HB_VEC_PMAXUW;
        case HB_INS_PMAXUD: return HB_VEC_PMAXUD;
        case HB_INS_PMULLD: return HB_VEC_PMULLD;
        case HB_INS_PHMINPOSUW: return HB_VEC_PHMINPOSUW;
        case HB_INS_PALIGNR: return HB_VEC_PALIGNR;
        case HB_INS_PBLENDW: return HB_VEC_PBLENDW;
        case HB_INS_BLENDPS: return HB_VEC_BLENDPS;
        case HB_INS_BLENDPD: return HB_VEC_BLENDPD;
        case HB_INS_PBLENDVB: return HB_VEC_PBLENDVB;
        case HB_INS_BLENDVPS: return HB_VEC_BLENDVPS;
        case HB_INS_BLENDVPD: return HB_VEC_BLENDVPD;
        case HB_INS_PSUBUSB: return HB_VEC_PSUBUSB;
        case HB_INS_PSUBUSW: return HB_VEC_PSUBUSW;
        case HB_INS_PSUBSB: return HB_VEC_PSUBSB;
        case HB_INS_PSUBSW: return HB_VEC_PSUBSW;
        case HB_INS_PMINUB: return HB_VEC_PMINUB;
        case HB_INS_PMINSW: return HB_VEC_PMINSW;
        case HB_INS_PMAXUB: return HB_VEC_PMAXUB;
        case HB_INS_PMAXSW: return HB_VEC_PMAXSW;
        case HB_INS_PMULUDQ: return HB_VEC_PMULUDQ;
        case HB_INS_PSADBW: return HB_VEC_PSADBW;
        case HB_INS_MPSADBW:
        case HB_INS_VMPSADBW: return HB_VEC_MPSADBW;
        case HB_INS_VPBROADCASTB: return HB_VEC_VPBROADCASTB;
        case HB_INS_VPBROADCASTW: return HB_VEC_VPBROADCASTW;
        case HB_INS_VPBROADCASTD: return HB_VEC_VPBROADCASTD;
        case HB_INS_VPBROADCASTQ: return HB_VEC_VPBROADCASTQ;
        case HB_INS_VBROADCASTSS: return HB_VEC_VBROADCASTSS;
        case HB_INS_VBROADCASTSD: return HB_VEC_VBROADCASTSD;
        case HB_INS_VBROADCASTF32X2: return HB_VEC_VBROADCASTF32X2;
        case HB_INS_VBROADCASTF64X2: return HB_VEC_VBROADCASTF64X2;
        case HB_INS_VBROADCASTF32X4: return HB_VEC_VBROADCASTF32X4;
        case HB_INS_VBROADCASTF64X4: return HB_VEC_VBROADCASTF64X4;
        case HB_INS_VBROADCASTF32X8: return HB_VEC_VBROADCASTF32X8;
        case HB_INS_VBROADCASTI32X2: return HB_VEC_VBROADCASTI32X2;
        case HB_INS_VBROADCASTI128: return HB_VEC_VBROADCASTI128;
        case HB_INS_VPBLENDD: return HB_VEC_VPBLENDD;
        case HB_INS_VPERMQ: return HB_VEC_VPERMQ;
        case HB_INS_VPERMPD: return HB_VEC_VPERMPD;
        case HB_INS_VPERMILPS: return HB_VEC_VPERMILPS;
        case HB_INS_VPERMILPD: return HB_VEC_VPERMILPD;
        case HB_INS_VBLENDVPS: return HB_VEC_VBLENDVPS;
        case HB_INS_VBLENDVPD: return HB_VEC_VBLENDVPD;
        case HB_INS_VPBLENDVB: return HB_VEC_VPBLENDVB;
        case HB_INS_VINSERTF128: return HB_VEC_VINSERTF128;
        case HB_INS_VINSERTI128: return HB_VEC_VINSERTI128;
        case HB_INS_VEXTRACTF128: return HB_VEC_VEXTRACTF128;
        case HB_INS_VEXTRACTI128: return HB_VEC_VEXTRACTI128;
        case HB_INS_VPERM2F128: return HB_VEC_VPERM2F128;
        case HB_INS_VPERM2I128: return HB_VEC_VPERM2I128;
        case HB_INS_VPSRLVD: return HB_VEC_VPSRLVD;
        case HB_INS_VPSRLVQ: return HB_VEC_VPSRLVQ;
        case HB_INS_VPSRAVD: return HB_VEC_VPSRAVD;
        case HB_INS_VPSLLVD: return HB_VEC_VPSLLVD;
        case HB_INS_VPSLLVQ: return HB_VEC_VPSLLVQ;
        case HB_INS_PCLMULQDQ: return HB_VEC_PCLMULQDQ;
        case HB_INS_VPCLMULQDQ: return HB_VEC_PCLMULQDQ;
        case HB_INS_AESKEYGENASSIST: return HB_VEC_AESKEYGENASSIST;
        case HB_INS_AESIMC: return HB_VEC_AESIMC;
        case HB_INS_AESENC:
        case HB_INS_VAESENC: return HB_VEC_AESENC;
        case HB_INS_AESENCLAST:
        case HB_INS_VAESENCLAST: return HB_VEC_AESENCLAST;
        case HB_INS_AESDEC:
        case HB_INS_VAESDEC: return HB_VEC_AESDEC;
        case HB_INS_AESDECLAST:
        case HB_INS_VAESDECLAST: return HB_VEC_AESDECLAST;
        case HB_INS_GF2P8MULB:
        case HB_INS_VGF2P8MULB: return HB_VEC_GF2P8MULB;
        case HB_INS_GF2P8AFFINEQB:
        case HB_INS_VGF2P8AFFINEQB: return HB_VEC_GF2P8AFFINEQB;
        case HB_INS_GF2P8AFFINEINVQB:
        case HB_INS_VGF2P8AFFINEINVQB: return HB_VEC_GF2P8AFFINEINVQB;
        case HB_INS_SHA1NEXTE: return HB_VEC_SHA1NEXTE;
        case HB_INS_SHA1MSG1: return HB_VEC_SHA1MSG1;
        case HB_INS_SHA1MSG2: return HB_VEC_SHA1MSG2;
        case HB_INS_SHA256RNDS2: return HB_VEC_SHA256RNDS2;
        case HB_INS_SHA256MSG1: return HB_VEC_SHA256MSG1;
        case HB_INS_SHA256MSG2: return HB_VEC_SHA256MSG2;
        case HB_INS_SHA1RNDS4: return HB_VEC_SHA1RNDS4;
        case HB_INS_VPERMD: return HB_VEC_VPERMD;
        case HB_INS_VPERMPS: return HB_VEC_VPERMPS;
        case HB_INS_VMASKMOVPS: return HB_VEC_VMASKMOVPS;
        case HB_INS_VMASKMOVPD: return HB_VEC_VMASKMOVPD;
        case HB_INS_VMASKMOVDQU: return HB_VEC_VMASKMOVDQU;
        case HB_INS_VPMASKMOVD: return HB_VEC_VPMASKMOVD;
        case HB_INS_VPMASKMOVQ: return HB_VEC_VPMASKMOVQ;
        case HB_INS_VGATHERDPS: return HB_VEC_VGATHERDPS;
        case HB_INS_VGATHERDPD: return HB_VEC_VGATHERDPD;
        case HB_INS_VGATHERQPS: return HB_VEC_VGATHERQPS;
        case HB_INS_VGATHERQPD: return HB_VEC_VGATHERQPD;
        case HB_INS_VPGATHERDD: return HB_VEC_VPGATHERDD;
        case HB_INS_VPGATHERDQ: return HB_VEC_VPGATHERDQ;
        case HB_INS_VPGATHERQD: return HB_VEC_VPGATHERQD;
        case HB_INS_VPGATHERQQ: return HB_VEC_VPGATHERQQ;
        case HB_INS_PCMPESTRM: return HB_VEC_PCMPESTRM;
        case HB_INS_PCMPESTRI: return HB_VEC_PCMPESTRI;
        case HB_INS_PCMPISTRM: return HB_VEC_PCMPISTRM;
        case HB_INS_PCMPISTRI: return HB_VEC_PCMPISTRI;
        case HB_INS_VFMADD132PS:
        case HB_INS_VFMADD132PD:
        case HB_INS_VFMADD132SS:
        case HB_INS_VFMADD132SD: return HB_VEC_VFMADD132;
        case HB_INS_VFMADD213PS:
        case HB_INS_VFMADD213PD:
        case HB_INS_VFMADD213SS:
        case HB_INS_VFMADD213SD: return HB_VEC_VFMADD213;
        case HB_INS_VFMADD231PS:
        case HB_INS_VFMADD231PD:
        case HB_INS_VFMADD231SS:
        case HB_INS_VFMADD231SD: return HB_VEC_VFMADD231;
        case HB_INS_VFMSUB132PS:
        case HB_INS_VFMSUB132PD:
        case HB_INS_VFMSUB132SS:
        case HB_INS_VFMSUB132SD: return HB_VEC_VFMSUB132;
        case HB_INS_VFMSUB213PS:
        case HB_INS_VFMSUB213PD:
        case HB_INS_VFMSUB213SS:
        case HB_INS_VFMSUB213SD: return HB_VEC_VFMSUB213;
        case HB_INS_VFMSUB231PS:
        case HB_INS_VFMSUB231PD:
        case HB_INS_VFMSUB231SS:
        case HB_INS_VFMSUB231SD: return HB_VEC_VFMSUB231;
        case HB_INS_VFMADDSUB132PS:
        case HB_INS_VFMADDSUB132PD: return HB_VEC_VFMADDSUB132;
        case HB_INS_VFMSUBADD132PS:
        case HB_INS_VFMSUBADD132PD: return HB_VEC_VFMSUBADD132;
        case HB_INS_VFMADDSUB213PS:
        case HB_INS_VFMADDSUB213PD: return HB_VEC_VFMADDSUB213;
        case HB_INS_VFMSUBADD213PS:
        case HB_INS_VFMSUBADD213PD: return HB_VEC_VFMSUBADD213;
        case HB_INS_VFMADDSUB231PS:
        case HB_INS_VFMADDSUB231PD: return HB_VEC_VFMADDSUB231;
        case HB_INS_VFMSUBADD231PS:
        case HB_INS_VFMSUBADD231PD: return HB_VEC_VFMSUBADD231;
        case HB_INS_VFNMADD132PS:
        case HB_INS_VFNMADD132PD:
        case HB_INS_VFNMADD132SS:
        case HB_INS_VFNMADD132SD: return HB_VEC_VFNMADD132;
        case HB_INS_VFNMSUB132PS:
        case HB_INS_VFNMSUB132PD:
        case HB_INS_VFNMSUB132SS:
        case HB_INS_VFNMSUB132SD: return HB_VEC_VFNMSUB132;
        case HB_INS_VFNMADD213PS:
        case HB_INS_VFNMADD213PD:
        case HB_INS_VFNMADD213SS:
        case HB_INS_VFNMADD213SD: return HB_VEC_VFNMADD213;
        case HB_INS_VFNMSUB213PS:
        case HB_INS_VFNMSUB213PD:
        case HB_INS_VFNMSUB213SS:
        case HB_INS_VFNMSUB213SD: return HB_VEC_VFNMSUB213;
        case HB_INS_VFNMADD231PS:
        case HB_INS_VFNMADD231PD:
        case HB_INS_VFNMADD231SS:
        case HB_INS_VFNMADD231SD: return HB_VEC_VFNMADD231;
        case HB_INS_VFNMSUB231PS:
        case HB_INS_VFNMSUB231PD:
        case HB_INS_VFNMSUB231SS:
        case HB_INS_VFNMSUB231SD: return HB_VEC_VFNMSUB231;
        case HB_INS_VCVTPH2PS: return HB_VEC_VCVTPH2PS;
        case HB_INS_VCVTPS2PH: return HB_VEC_VCVTPS2PH;
        case HB_INS_VCMPPS: return HB_VEC_VCMPPS;
        case HB_INS_VCMPPD: return HB_VEC_VCMPPD;
        case HB_INS_VCMPSS: return HB_VEC_VCMPSS;
        case HB_INS_VCMPSD: return HB_VEC_VCMPSD;
        case HB_INS_VHADDPS: return HB_VEC_VHADDPS;
        case HB_INS_VHADDPD: return HB_VEC_VHADDPD;
        case HB_INS_VHSUBPS: return HB_VEC_VHSUBPS;
        case HB_INS_VHSUBPD: return HB_VEC_VHSUBPD;
        case HB_INS_VADDSUBPS: return HB_VEC_VADDSUBPS;
        case HB_INS_VADDSUBPD: return HB_VEC_VADDSUBPD;
        case HB_INS_VMOVSLDUP: return HB_VEC_VMOVSLDUP;
        case HB_INS_VMOVSHDUP: return HB_VEC_VMOVSHDUP;
        case HB_INS_VMOVDDUP: return HB_VEC_VMOVDDUP;
        default: return 0;
    }
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
    if (i) {
        i->guest_addr = dec->addr;
        i->guest_len = dec->len;
        if (is_vex_decoded(dec) && dec->op1.is_reg &&
            dec->op1.reg >= HB_REG_XMM0 && dec->op1.reg <= HB_REG_XMM31 &&
            dec->op1.size == 16) {
            i->zero_ymm_upper = true;
        }
    }
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
        case HB_INS_MOVDIRI: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_store(b, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_MOVDIR64B: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVDIR64B);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOV_SEG: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_unop(b, HB_IR_MOV_SEG, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_SSE_MOV:
        case HB_INS_MOVNTDQA: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            unsigned evex_lane = dec->evex_mask_lane ? dec->evex_mask_lane : 0;
            if (dec->op1.is_mem && dec->op2.is_reg) {
                hb_ir_instr_t* i = emit(b, hb_ir_emit_store(b, dst, src), dec);
                set_evex_target_arg(i, dec, evex_lane);
            } else if (is_vex_decoded(dec) && dec->op1.is_reg &&
                       dec->op1.reg >= HB_REG_XMM0 && dec->op1.reg <= HB_REG_XMM31 &&
                       ((dec->op2.is_mem && (dec->op2.size == 4 || dec->op2.size == 8)) ||
                        (dec->op3.present && (dec->op3.size == 4 || dec->op3.size == 8)))) {
                hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_XMM_SCALAR_MOV);
                if (i) {
                    i->dst = dst;
                    i->src1 = dec->op3.present ? src : hb_ir_none();
                    i->src2 = dec->op3.present ? operand_from_dec(dec, 3) : src;
                    i->target = evex_target_arg(dec, dec->op3.present ? dec->op3.size : dec->op2.size);
                    if (!dec->op3.present && dec->op2.is_mem) i->zero_upper = true;
                }
                emit(b, i, dec);
            } else if (dec->op1.is_reg && dec->op2.is_mem) {
                hb_ir_instr_t* i = emit(b, hb_ir_emit_load(b, dst, src), dec);
                if (i) {
                    i->zero_upper = is_legacy_scalar_sse_mem_load(dec);
                    set_evex_target_arg(i, dec, evex_lane);
                }
            } else {
                hb_ir_instr_t* i = emit(b, hb_ir_emit_mov(b, dst, src), dec);
                set_evex_target_arg(i, dec, evex_lane);
            }
            return HB_OK;
        }
        case HB_INS_VMOVHLPS:
        case HB_INS_VMOVLHPS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = operand_from_dec(dec, 2);
            hb_ir_instr_t *copy = hb_ir_emit_mov(b, dst, src1);
            emit(b, copy, dec);
            hb_ir_instr_t *lane = hb_ir_emit(b, HB_IR_XMM_QWORD_LANE_MOV);
            if (lane) {
                unsigned dst_lane = dec->opcode == HB_INS_VMOVLHPS ? 1 : 0;
                unsigned src_lane = dec->opcode == HB_INS_VMOVHLPS ? 1 : 0;
                lane->dst = dst;
                lane->src1 = operand_from_dec(dec, 3);
                lane->target = dst_lane | (src_lane << 8);
                lane->zero_ymm_upper = true;
            }
            emit(b, lane, dec);
            return HB_OK;
        }
        case HB_INS_VMOVLPS:
        case HB_INS_VMOVHPS:
        case HB_INS_VMOVLPD:
        case HB_INS_VMOVHPD: {
            bool store = dec->op1.is_mem;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            unsigned lane_id = (dec->opcode == HB_INS_VMOVHPS ||
                                dec->opcode == HB_INS_VMOVHPD) ? 1 : 0;
            if (!store) {
                hb_ir_instr_t *copy = hb_ir_emit_mov(b, dst, src);
                emit(b, copy, dec);
            }
            hb_ir_instr_t *lane = hb_ir_emit(b, HB_IR_XMM_QWORD_LANE_MOV);
            if (lane) {
                lane->dst = dst;
                lane->src1 = store ? src : operand_from_dec(dec, 3);
                lane->target = lane_id | (lane_id << 8);
                lane->zero_ymm_upper = !store;
            }
            emit(b, lane, dec);
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
        case HB_INS_CVTSI2SD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTSI2SD);
            if (i) {
                i->dst = dst;
                if (is_vex_decoded(dec) && dec->op3.present) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = hb_ir_none();
                }
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTSI2SS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTSI2SS);
            if (i) {
                i->dst = dst;
                if (is_vex_decoded(dec) && dec->op3.present) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = hb_ir_none();
                }
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MULSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, is_vex_decoded(dec) ? HB_IR_FMUL : HB_IR_MULSD);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; if (is_vex_decoded(dec)) i->target = dec->evex ? evex_target_arg(dec, 8 | 0x100) : (8 | 0x100); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MULSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, is_vex_decoded(dec) ? HB_IR_FMUL : HB_IR_MULSS);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; if (is_vex_decoded(dec)) i->target = dec->evex ? evex_target_arg(dec, 4 | 0x100) : (4 | 0x100); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DIVSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, is_vex_decoded(dec) ? HB_IR_FDIV : HB_IR_DIVSS);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; if (is_vex_decoded(dec)) i->target = dec->evex ? evex_target_arg(dec, 4 | 0x100) : (4 | 0x100); }
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                uint64_t arg = lane | (scalar ? 0x100 : 0);
                if (dec->evex_broadcast) arg |= HB_EVEX_ARG_BROADCAST;
                i->target = evex_target_arg(dec, arg);
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
        case HB_INS_CVTSD2SI:
        case HB_INS_CVTTSD2SI: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_CVTSD2SI ? HB_IR_CVTSD2SI : HB_IR_CVTTSD2SI);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTSS2SI:
        case HB_INS_CVTTSS2SI: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_CVTSS2SI ? HB_IR_CVTSS2SI : HB_IR_CVTTSS2SI);
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_op_t op = HB_IR_XORPS;
            if (dec->opcode == HB_INS_XMM_AND) op = HB_IR_XMM_AND;
            else if (dec->opcode == HB_INS_XMM_ANDN) op = HB_IR_XMM_ANDN;
            else if (dec->opcode == HB_INS_XMM_OR) op = HB_IR_XMM_OR;
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                unsigned lane = dec->evex_mask_lane ? dec->evex_mask_lane : 4;
                uint64_t arg = lane;
                if (dec->evex_broadcast) arg |= HB_EVEX_ARG_BROADCAST;
                i->dst = dst; i->src1 = src1; i->src2 = src2; set_evex_target_arg(i, dec, arg);
            }
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; }
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; }
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
                i->src1 = vector_src1_from_dec(dec, i->dst);
                i->src2 = vector_src2_from_dec(dec);
                i->target = evex_target_arg(dec, (packed_double ? 8 : 4) | (high ? 0x100 : 0));
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                i->target = evex_target_arg(dec, lane | (high ? 0x100 : 0));
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; i->target = evex_target_arg(dec, 0); }
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; i->target = evex_target_arg(dec, 0); }
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
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; i->target = evex_target_arg(dec, 0); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PAVGB:
        case HB_INS_PAVGW: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_PAVGB ? HB_IR_PAVGB : HB_IR_PAVGW);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; i->target = evex_target_arg(dec, 0); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSHUFB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src1 = vector_src1_from_dec(dec, dst);
            hb_ir_operand_t src2 = vector_src2_from_dec(dec);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PSHUFB);
            if (i) { i->dst = dst; i->src1 = src1; i->src2 = src2; i->target = evex_target_arg(dec, 0); }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PHADDW:
        case HB_INS_PHADDD:
        case HB_INS_PHADDSW:
        case HB_INS_PHSUBW:
        case HB_INS_PHSUBD:
        case HB_INS_PHSUBSW:
        case HB_INS_PMADDUBSW:
        case HB_INS_PSIGNB:
        case HB_INS_PSIGNW:
        case HB_INS_PSIGND:
        case HB_INS_PMULHRSW:
        case HB_INS_PABSB:
        case HB_INS_PABSW:
        case HB_INS_PABSD:
        case HB_INS_PTEST:
        case HB_INS_PMOVSXBW:
        case HB_INS_PMOVSXBD:
        case HB_INS_PMOVSXBQ:
        case HB_INS_PMOVSXWD:
        case HB_INS_PMOVSXWQ:
        case HB_INS_PMOVSXDQ:
        case HB_INS_PMULDQ:
        case HB_INS_PCMPEQQ:
        case HB_INS_PACKUSDW:
        case HB_INS_PMOVZXBW:
        case HB_INS_PMOVZXBD:
        case HB_INS_PMOVZXBQ:
        case HB_INS_PMOVZXWD:
        case HB_INS_PMOVZXWQ:
        case HB_INS_PMOVZXDQ:
        case HB_INS_PCMPGTQ:
        case HB_INS_PMINSB:
        case HB_INS_PMINSD:
        case HB_INS_PMINUW:
        case HB_INS_PMINUD:
        case HB_INS_PMAXSB:
        case HB_INS_PMAXSD:
        case HB_INS_PMAXUW:
        case HB_INS_PMAXUD:
        case HB_INS_PMULLD:
        case HB_INS_PHMINPOSUW:
        case HB_INS_MPSADBW:
        case HB_INS_PALIGNR:
        case HB_INS_PBLENDW:
        case HB_INS_BLENDPS:
        case HB_INS_BLENDPD:
        case HB_INS_PBLENDVB:
        case HB_INS_BLENDVPS:
        case HB_INS_BLENDVPD:
        case HB_INS_PSUBUSB:
        case HB_INS_PSUBUSW:
        case HB_INS_PSUBSB:
        case HB_INS_PSUBSW:
        case HB_INS_PMINUB:
        case HB_INS_PMINSW:
        case HB_INS_PMAXUB:
        case HB_INS_PMAXSW:
        case HB_INS_PMULUDQ:
        case HB_INS_PSADBW:
        case HB_INS_VPBROADCASTB:
        case HB_INS_VPBROADCASTW:
        case HB_INS_VPBROADCASTD:
        case HB_INS_VPBROADCASTQ:
        case HB_INS_VBROADCASTSS:
        case HB_INS_VBROADCASTSD:
        case HB_INS_VBROADCASTF32X2:
        case HB_INS_VBROADCASTF64X2:
        case HB_INS_VBROADCASTF32X4:
        case HB_INS_VBROADCASTF64X4:
        case HB_INS_VBROADCASTF32X8:
        case HB_INS_VBROADCASTI32X2:
        case HB_INS_VBROADCASTI128:
        case HB_INS_VPBLENDD:
        case HB_INS_VPERMQ:
        case HB_INS_VPERMPD:
        case HB_INS_VPERMILPS:
        case HB_INS_VPERMILPD:
        case HB_INS_VBLENDVPS:
        case HB_INS_VBLENDVPD:
        case HB_INS_VPBLENDVB:
        case HB_INS_VINSERTF128:
        case HB_INS_VINSERTI128:
        case HB_INS_VEXTRACTF128:
        case HB_INS_VEXTRACTI128:
        case HB_INS_VPERM2F128:
        case HB_INS_VPERM2I128:
        case HB_INS_VPSRLVD:
        case HB_INS_VPSRLVQ:
        case HB_INS_VPSRAVD:
        case HB_INS_VPSLLVD:
        case HB_INS_VPSLLVQ:
        case HB_INS_PCLMULQDQ:
        case HB_INS_VPCLMULQDQ:
        case HB_INS_AESKEYGENASSIST:
        case HB_INS_AESIMC:
        case HB_INS_AESENC:
        case HB_INS_AESENCLAST:
        case HB_INS_AESDEC:
        case HB_INS_AESDECLAST:
        case HB_INS_VAESENC:
        case HB_INS_VAESENCLAST:
        case HB_INS_VAESDEC:
        case HB_INS_VAESDECLAST:
        case HB_INS_GF2P8MULB:
        case HB_INS_VGF2P8MULB:
        case HB_INS_GF2P8AFFINEQB:
        case HB_INS_GF2P8AFFINEINVQB:
        case HB_INS_VGF2P8AFFINEQB:
        case HB_INS_VGF2P8AFFINEINVQB:
        case HB_INS_VMPSADBW:
        case HB_INS_VPERMD:
        case HB_INS_VPERMPS:
        case HB_INS_VMASKMOVPS:
        case HB_INS_VMASKMOVPD:
        case HB_INS_VMASKMOVDQU:
        case HB_INS_VPMASKMOVD:
        case HB_INS_VPMASKMOVQ:
        case HB_INS_VGATHERDPS:
        case HB_INS_VGATHERDPD:
        case HB_INS_VGATHERQPS:
        case HB_INS_VGATHERQPD:
        case HB_INS_VPGATHERDD:
        case HB_INS_VPGATHERDQ:
        case HB_INS_VPGATHERQD:
        case HB_INS_VPGATHERQQ:
        case HB_INS_PCMPESTRM:
        case HB_INS_PCMPESTRI:
        case HB_INS_PCMPISTRM:
        case HB_INS_PCMPISTRI:
        case HB_INS_VTESTPS:
        case HB_INS_VTESTPD:
        case HB_INS_VFMADD132PS:
        case HB_INS_VFMADD132PD:
        case HB_INS_VFMADD132SS:
        case HB_INS_VFMADD132SD:
        case HB_INS_VFMADD213PS:
        case HB_INS_VFMADD213PD:
        case HB_INS_VFMADD213SS:
        case HB_INS_VFMADD213SD:
        case HB_INS_VFMADD231PS:
        case HB_INS_VFMADD231PD:
        case HB_INS_VFMADD231SS:
        case HB_INS_VFMADD231SD:
        case HB_INS_VFMSUB132PS:
        case HB_INS_VFMSUB132PD:
        case HB_INS_VFMSUB132SS:
        case HB_INS_VFMSUB132SD:
        case HB_INS_VFMSUB213PS:
        case HB_INS_VFMSUB213PD:
        case HB_INS_VFMSUB213SS:
        case HB_INS_VFMSUB213SD:
        case HB_INS_VFMSUB231PS:
        case HB_INS_VFMSUB231PD:
        case HB_INS_VFMSUB231SS:
        case HB_INS_VFMSUB231SD:
        case HB_INS_VFMADDSUB132PS:
        case HB_INS_VFMADDSUB132PD:
        case HB_INS_VFMSUBADD132PS:
        case HB_INS_VFMSUBADD132PD:
        case HB_INS_VFMADDSUB213PS:
        case HB_INS_VFMADDSUB213PD:
        case HB_INS_VFMSUBADD213PS:
        case HB_INS_VFMSUBADD213PD:
        case HB_INS_VFMADDSUB231PS:
        case HB_INS_VFMADDSUB231PD:
        case HB_INS_VFMSUBADD231PS:
        case HB_INS_VFMSUBADD231PD:
        case HB_INS_VFNMADD132PS:
        case HB_INS_VFNMADD132PD:
        case HB_INS_VFNMADD132SS:
        case HB_INS_VFNMADD132SD:
        case HB_INS_VFNMSUB132PS:
        case HB_INS_VFNMSUB132PD:
        case HB_INS_VFNMSUB132SS:
        case HB_INS_VFNMSUB132SD:
        case HB_INS_VFNMADD213PS:
        case HB_INS_VFNMADD213PD:
        case HB_INS_VFNMADD213SS:
        case HB_INS_VFNMADD213SD:
        case HB_INS_VFNMSUB213PS:
        case HB_INS_VFNMSUB213PD:
        case HB_INS_VFNMSUB213SS:
        case HB_INS_VFNMSUB213SD:
        case HB_INS_VFNMADD231PS:
        case HB_INS_VFNMADD231PD:
        case HB_INS_VFNMADD231SS:
        case HB_INS_VFNMADD231SD:
        case HB_INS_VFNMSUB231PS:
        case HB_INS_VFNMSUB231PD:
        case HB_INS_VFNMSUB231SS:
        case HB_INS_VFNMSUB231SD:
        case HB_INS_VCVTPH2PS:
        case HB_INS_VCVTPS2PH:
        case HB_INS_VCMPPS:
        case HB_INS_VCMPPD:
        case HB_INS_VCMPSS:
        case HB_INS_VCMPSD:
        case HB_INS_VHADDPS:
        case HB_INS_VHADDPD:
        case HB_INS_VHSUBPS:
        case HB_INS_VHSUBPD:
        case HB_INS_VADDSUBPS:
        case HB_INS_VADDSUBPD:
        case HB_INS_VMOVSLDUP:
        case HB_INS_VMOVSHDUP:
        case HB_INS_VMOVDDUP:
        case HB_INS_SHA1NEXTE:
        case HB_INS_SHA1MSG1:
        case HB_INS_SHA1MSG2:
        case HB_INS_SHA256RNDS2:
        case HB_INS_SHA256MSG1:
        case HB_INS_SHA256MSG2:
        case HB_INS_SHA1RNDS4: {
            if (dec->evex && (dec->opcode == HB_INS_VCMPPS || dec->opcode == HB_INS_VCMPPD ||
                              dec->opcode == HB_INS_VCMPSS || dec->opcode == HB_INS_VCMPSD)) {
                hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_EVEX_CMP_MASK);
                if (i) {
                    bool is_pd = dec->opcode == HB_INS_VCMPPD || dec->opcode == HB_INS_VCMPSD;
                    bool scalar = dec->opcode == HB_INS_VCMPSS || dec->opcode == HB_INS_VCMPSD;
                    uint64_t kdst = dec->op1.is_imm ? ((uint64_t)dec->op1.imm & 7u) : 0;
                    uint64_t kmask = dec->evex_mask & 7u;
                    uint64_t pred = dec_imm8(dec) & 31u;
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                    i->target = kdst | (kmask << 3) | (scalar ? (1u << 6) : 0) |
                                (is_pd ? (1u << 7) : 0) | (dec->evex_broadcast ? (1u << 8) : 0) |
                                (pred << 16);
                }
                emit(b, i, dec);
                return HB_OK;
            }
            hb_ir_vec_op_t op = vec_op_from_ins(dec->opcode);
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_VEC_PACKED);
            if (i) {
                uint64_t imm = dec_imm8(dec);
                i->dst = dst;
                i->src1 = hb_ir_none();
                i->src2 = hb_ir_none();
                if (dec->opcode == HB_INS_PTEST ||
                    dec->opcode == HB_INS_VTESTPS || dec->opcode == HB_INS_VTESTPD) {
                    i->src1 = dst;
                    i->src2 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_VMASKMOVDQU) {
                    i->src1 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_MPSADBW ||
                           dec->opcode == HB_INS_PALIGNR || dec->opcode == HB_INS_PBLENDW ||
                           dec->opcode == HB_INS_BLENDPS || dec->opcode == HB_INS_BLENDPD) {
                    i->src1 = dst;
                    i->src2 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_PABSB || dec->opcode == HB_INS_PABSW ||
                           dec->opcode == HB_INS_PABSD) {
                    i->src1 = vector_src2_from_dec(dec);
                } else if ((dec->opcode >= HB_INS_PMOVSXBW && dec->opcode <= HB_INS_PMOVSXDQ) ||
                            (dec->opcode >= HB_INS_PMOVZXBW && dec->opcode <= HB_INS_PMOVZXDQ) ||
                            dec->opcode == HB_INS_PHMINPOSUW ||
                            (dec->opcode >= HB_INS_VPBROADCASTB && dec->opcode <= HB_INS_VBROADCASTI128) ||
                            dec->opcode == HB_INS_VMOVSLDUP ||
                            dec->opcode == HB_INS_VMOVSHDUP ||
                            dec->opcode == HB_INS_VMOVDDUP ||
                            dec->opcode == HB_INS_AESKEYGENASSIST ||
                            dec->opcode == HB_INS_AESIMC) {
                    i->src1 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_VPERMQ || dec->opcode == HB_INS_VPERMPD ||
                           dec->opcode == HB_INS_VEXTRACTF128 || dec->opcode == HB_INS_VEXTRACTI128 ||
                           ((dec->opcode == HB_INS_VPERMILPS || dec->opcode == HB_INS_VPERMILPD) &&
                            dec->op3.is_imm)) {
                    i->src1 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_VPBLENDD ||
                           dec->opcode == HB_INS_VBLENDVPS || dec->opcode == HB_INS_VBLENDVPD ||
                           dec->opcode == HB_INS_VPBLENDVB ||
                           dec->opcode == HB_INS_VINSERTF128 || dec->opcode == HB_INS_VINSERTI128 ||
                           dec->opcode == HB_INS_VPERM2F128 || dec->opcode == HB_INS_VPERM2I128 ||
                           dec->opcode == HB_INS_VPSRLVD || dec->opcode == HB_INS_VPSRLVQ ||
                           dec->opcode == HB_INS_VPSRAVD ||
                           dec->opcode == HB_INS_VPSLLVD || dec->opcode == HB_INS_VPSLLVQ ||
                           dec->opcode == HB_INS_VPCLMULQDQ ||
                            dec->opcode == HB_INS_VPERMD || dec->opcode == HB_INS_VPERMPS ||
                            dec->opcode == HB_INS_VPERMILPS || dec->opcode == HB_INS_VPERMILPD ||
                            dec->opcode == HB_INS_VCMPPS || dec->opcode == HB_INS_VCMPPD ||
                            dec->opcode == HB_INS_VCMPSS || dec->opcode == HB_INS_VCMPSD ||
                            dec->opcode == HB_INS_VMASKMOVPS || dec->opcode == HB_INS_VMASKMOVPD ||
                            dec->opcode == HB_INS_VPMASKMOVD || dec->opcode == HB_INS_VPMASKMOVQ ||
                            dec->opcode == HB_INS_VAESENC || dec->opcode == HB_INS_VAESENCLAST ||
                           dec->opcode == HB_INS_VAESDEC || dec->opcode == HB_INS_VAESDECLAST ||
                           dec->opcode == HB_INS_VGF2P8MULB ||
                           dec->opcode == HB_INS_VGF2P8AFFINEQB ||
                           dec->opcode == HB_INS_VGF2P8AFFINEINVQB ||
                           dec->opcode == HB_INS_VMPSADBW ||
                            (dec->opcode >= HB_INS_VGATHERDPS && dec->opcode <= HB_INS_VPGATHERQQ)) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else if (dec->opcode >= HB_INS_VFMADD132PS &&
                           dec->opcode <= HB_INS_VFNMSUB231SD) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                    imm = fma_target_from_ins(dec->opcode);
                } else if (dec->opcode == HB_INS_VCVTPH2PS || dec->opcode == HB_INS_VCVTPS2PH) {
                    i->src1 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_PCMPESTRM || dec->opcode == HB_INS_PCMPESTRI ||
                           dec->opcode == HB_INS_PCMPISTRM || dec->opcode == HB_INS_PCMPISTRI) {
                    i->dst = (dec->opcode == HB_INS_PCMPESTRI || dec->opcode == HB_INS_PCMPISTRI)
                           ? hb_ir_reg(HB_REG_RCX, HB_SIZE_32)
                           : hb_ir_reg(HB_REG_XMM0, HB_SIZE_128);
                    i->src1 = operand_from_dec(dec, 1);
                    i->src2 = operand_from_dec(dec, 2);
                } else if (dec->opcode == HB_INS_AESENC || dec->opcode == HB_INS_AESENCLAST ||
                           dec->opcode == HB_INS_AESDEC || dec->opcode == HB_INS_AESDECLAST ||
                           dec->opcode == HB_INS_GF2P8MULB ||
                           dec->opcode == HB_INS_GF2P8AFFINEQB ||
                           dec->opcode == HB_INS_GF2P8AFFINEINVQB) {
                    i->src1 = dst;
                    i->src2 = operand_from_dec(dec, 2);
                } else {
                    i->src1 = vector_src1_from_dec(dec, dst);
                    i->src2 = vector_src2_from_dec(dec);
                }
                i->target = vec_target(op, evex_target_arg(dec, imm));
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_VZEROUPPER:
            emit(b, hb_ir_emit(b, HB_IR_VZEROUPPER), dec);
            return HB_OK;
        case HB_INS_PSHUFD:
        case HB_INS_PSHUFLW:
        case HB_INS_PSHUFHW: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PSHUF);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->src2 = operand_from_dec(dec, 3);
                uint64_t target = dec->opcode == HB_INS_PSHUFD ? 4 :
                                  (dec->opcode == HB_INS_PSHUFLW ? 2 : 0x102);
                i->target = evex_target_arg(dec, target);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_SHUFPS:
        case HB_INS_SHUFPD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_FSHUF);
            if (i) {
                hb_ir_operand_t dst = operand_from_dec(dec, 1);
                i->dst = dst;
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = vector_src2_from_dec(dec);
                i->target = evex_target_arg(dec, (dec->opcode == HB_INS_SHUFPD ? 8 : 4) |
                                                (((uint64_t)dec_imm8(dec) & 0xffu) << 8));
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
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->dst = dst;
                if (is_vex_decoded(dec) && dec->op3.is_imm) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else {
                    i->src1 = vector_src1_from_dec(dec, dst);
                    i->src2 = vector_src2_from_dec(dec);
                }
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
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->dst = dst;
                if (is_vex_decoded(dec) && dec->op3.is_imm) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else {
                    i->src1 = vector_src1_from_dec(dec, dst);
                    i->src2 = vector_src2_from_dec(dec);
                }
            }
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
                i->src1 = vector_src1_from_dec(dec, i->dst);
                i->src2 = vector_src2_from_dec(dec);
                i->target = evex_target_arg(dec, lane);
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
                i->src1 = vector_src1_from_dec(dec, i->dst);
                i->src2 = vector_src2_from_dec(dec);
                i->target = evex_target_arg(dec, lane);
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
            hb_size_t sz = size_from_dec(dec->op1.size);
            hb_ir_operand_t sp = hb_ir_reg(HB_REG_RSP, sz);
            hb_ir_operand_t bp_same_size = hb_ir_reg(HB_REG_RBP, sz);
            hb_ir_operand_t bp = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_mov(b, sp, bp_same_size), dec);
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
        case HB_INS_POPCNT: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_POPCNT);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CRC32:
        case HB_INS_ANDN:
        case HB_INS_BEXTR:
        case HB_INS_BLSI:
        case HB_INS_BLSMSK:
        case HB_INS_BLSR:
        case HB_INS_BZHI:
        case HB_INS_PDEP:
        case HB_INS_PEXT:
        case HB_INS_RORX:
        case HB_INS_SARX:
        case HB_INS_SHLX:
        case HB_INS_SHRX:
        case HB_INS_ADCX:
        case HB_INS_ADOX: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_op_t op = dec->opcode == HB_INS_CRC32 ? HB_IR_CRC32 :
                            dec->opcode == HB_INS_ANDN ? HB_IR_ANDN :
                            dec->opcode == HB_INS_BEXTR ? HB_IR_BEXTR :
                            dec->opcode == HB_INS_BLSI ? HB_IR_BLSI :
                            dec->opcode == HB_INS_BLSMSK ? HB_IR_BLSMSK :
                            dec->opcode == HB_INS_BLSR ? HB_IR_BLSR :
                            dec->opcode == HB_INS_BZHI ? HB_IR_BZHI :
                            dec->opcode == HB_INS_PDEP ? HB_IR_PDEP :
                            dec->opcode == HB_INS_PEXT ? HB_IR_PEXT :
                            dec->opcode == HB_INS_RORX ? HB_IR_RORX :
                            dec->opcode == HB_INS_SARX ? HB_IR_SARX :
                            dec->opcode == HB_INS_SHLX ? HB_IR_SHLX :
                            dec->opcode == HB_INS_SHRX ? HB_IR_SHRX :
                            dec->opcode == HB_INS_ADCX ? HB_IR_ADCX : HB_IR_ADOX;
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->dst = dst;
                i->src1 = src;
                i->src2 = operand_from_dec(dec, 3);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MULX: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MULX);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->src2 = operand_from_dec(dec, 3);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_BSWAP: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_BSWAP);
            if (i) i->dst = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVBE: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVBE);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = dec->op1.size ? dec->op1.size : dec->op2.size;
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
        case HB_INS_MOVD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVD);
            if (i) {
                i->dst = dst;
                i->src1 = src;
                i->zero_ymm_upper = is_vex_decoded(dec) && dst.type == HB_OP_REG &&
                                    dst.reg >= HB_REG_XMM0 && dst.reg <= HB_REG_XMM31;
            }
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
        case HB_INS_CVTPD2DQ:
        case HB_INS_CVTTPD2DQ:
        case HB_INS_CVTSS2SD:
        case HB_INS_CVTSD2SS: {
            hb_ir_op_t op = HB_IR_CVTPS2PD;
            if (dec->opcode == HB_INS_CVTPD2PS) op = HB_IR_CVTPD2PS;
            else if (dec->opcode == HB_INS_CVTPD2DQ) op = HB_IR_CVTPD2DQ;
            else if (dec->opcode == HB_INS_CVTTPD2DQ) op = HB_IR_CVTTPD2DQ;
            else if (dec->opcode == HB_INS_CVTSS2SD) op = HB_IR_CVTSS2SD;
            else if (dec->opcode == HB_INS_CVTSD2SS) op = HB_IR_CVTSD2SS;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->dst = dst;
                if (is_vex_decoded(dec) && dec->op3.present) {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = operand_from_dec(dec, 3);
                } else {
                    i->src1 = operand_from_dec(dec, 2);
                    i->src2 = hb_ir_none();
                }
            }
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                uint64_t arg = lane | (scalar ? 0x100 : 0);
                if (dec->evex_broadcast) arg |= HB_EVEX_ARG_BROADCAST;
                if (dec->evex_rounding) arg |= ((uint64_t)(dec->evex_rounding & 7u) << HB_EVEX_ARG_ROUND_SHIFT);
                i->target = evex_target_arg(dec, arg);
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                uint64_t arg = lane;
                if (dec->evex_broadcast) arg |= HB_EVEX_ARG_BROADCAST;
                i->target = evex_target_arg(dec, arg);
            }
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                i->target = evex_target_arg(dec, lane | (scalar ? 0x100 : 0));
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
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                i->target = 4 | (scalar ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_ROUNDPS:
        case HB_INS_ROUNDPD:
        case HB_INS_ROUNDSS:
        case HB_INS_ROUNDSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            bool scalar = dec->opcode == HB_INS_ROUNDSS || dec->opcode == HB_INS_ROUNDSD;
            unsigned lane = (dec->opcode == HB_INS_ROUNDPD || dec->opcode == HB_INS_ROUNDSD) ? 8 : 4;
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_FROUND);
            if (i) {
                i->dst = dst;
                i->src1 = vector_src1_from_dec(dec, dst);
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                i->target = lane | (scalar ? 0x100 : 0) | ((uint64_t)dec_imm8(dec) << 16);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DPPS:
        case HB_INS_DPPD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            unsigned lane = dec->opcode == HB_INS_DPPD ? 8 : 4;
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_FDP);
            if (i) {
                i->dst = dst;
                i->src1 = has_vex_src(dec) ? operand_from_dec(dec, 2) : dst;
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : src;
                i->target = lane | ((uint64_t)dec_imm8(dec) << 16);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_INSERTPS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_INSERTPS);
            if (i) {
                i->dst = dst;
                i->src1 = has_vex_src(dec) ? operand_from_dec(dec, 2) : dst;
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : operand_from_dec(dec, 2);
                i->target = dec_imm8(dec);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_EXTRACTPS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_EXTRACTPS);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = dec_imm8(dec);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PINSRB:
        case HB_INS_PINSRW:
        case HB_INS_PINSRD:
        case HB_INS_PINSRQ: {
            unsigned elem_size = 4;
            if (dec->opcode == HB_INS_PINSRB) elem_size = 1;
            else if (dec->opcode == HB_INS_PINSRW) elem_size = 2;
            else if (dec->opcode == HB_INS_PINSRQ) elem_size = 8;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PINSR);
            if (i) {
                i->dst = dst;
                i->src1 = has_vex_src(dec) ? operand_from_dec(dec, 2) : dst;
                i->src2 = has_vex_src(dec) ? operand_from_dec(dec, 3) : operand_from_dec(dec, 2);
                i->target = elem_size | ((uint64_t)dec_imm8(dec) << 8);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PEXTRB:
        case HB_INS_PEXTRW:
        case HB_INS_PEXTRD:
        case HB_INS_PEXTRQ: {
            unsigned elem_size = 4;
            if (dec->opcode == HB_INS_PEXTRB) elem_size = 1;
            else if (dec->opcode == HB_INS_PEXTRW) elem_size = 2;
            else if (dec->opcode == HB_INS_PEXTRQ) elem_size = 8;
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PEXTR);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->target = elem_size | ((uint64_t)dec_imm8(dec) << 8);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DIVSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, is_vex_decoded(dec) ? HB_IR_FDIV : HB_IR_DIVSD);
            if (i) {
                i->dst = dst;
                i->src1 = is_vex_decoded(dec) ? vector_src1_from_dec(dec, dst) : dst;
                i->src2 = is_vex_decoded(dec) ? vector_src2_from_dec(dec) : src;
                if (is_vex_decoded(dec)) i->target = dec->evex ? evex_target_arg(dec, 8 | 0x100) : (8 | 0x100);
            }
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
        case HB_INS_X87_FNCLEX:
        case HB_INS_X87_FNINIT:
            emit(b, hb_ir_emit(b, HB_IR_NOP), dec);
            return HB_OK;
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
            emit(b, hb_ir_emit(b, HB_IR_X87_FCOMPP), dec);
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
        /* x87 FCMOVcc / FFREE / FFREEP — the x64 decoder doesn't produce
         * these today (they're rare on 64-bit code) but the lifter should
         * still handle them in case the decoder is ever extended. Until
         * then the existing FCMOV/FFREEP NOP stand-ins keep the build
         * green. Listed here only as documentation; the actual handlers
         * remain above. */
        /* FUCOM family — gap matrix #8 fix. */
        case HB_INS_X87_FUCOM:
            emit(b, hb_ir_emit(b, HB_IR_X87_FUCOM), dec);
            return HB_OK;
        case HB_INS_X87_FUCOMP:
            emit(b, hb_ir_emit(b, HB_IR_X87_FUCOMP), dec);
            return HB_OK;
        case HB_INS_X87_FCOMI:
            emit(b, hb_ir_emit(b, HB_IR_X87_FCOMI), dec);
            return HB_OK;
        case HB_INS_X87_FUCOMI:
            emit(b, hb_ir_emit(b, HB_IR_X87_FUCOMI), dec);
            return HB_OK;
        case HB_INS_X87_FCOMPI:  /* decoder alias for FCOMIP */
            emit(b, hb_ir_emit(b, HB_IR_X87_FCOMIP), dec);
            return HB_OK;
        case HB_INS_X87_FUCOMPI: /* decoder alias for FUCOMIP */
            emit(b, hb_ir_emit(b, HB_IR_X87_FUCOMIP), dec);
            return HB_OK;
        /* D9 F0-FF transcendentals — gap matrix #6. */
        case HB_INS_X87_FSQRT:   emit(b, hb_ir_emit(b, HB_IR_X87_FSQRT), dec);   return HB_OK;
        case HB_INS_X87_F2XM1:   emit(b, hb_ir_emit(b, HB_IR_X87_F2XM1), dec);   return HB_OK;
        case HB_INS_X87_FYL2X:   emit(b, hb_ir_emit(b, HB_IR_X87_FYL2X), dec);   return HB_OK;
        case HB_INS_X87_FPTAN:   emit(b, hb_ir_emit(b, HB_IR_X87_FPTAN), dec);   return HB_OK;
        case HB_INS_X87_FPATAN:  emit(b, hb_ir_emit(b, HB_IR_X87_FPATAN), dec);  return HB_OK;
        case HB_INS_X87_FXTRACT: emit(b, hb_ir_emit(b, HB_IR_X87_FXTRACT), dec); return HB_OK;
        case HB_INS_X87_FPREM1:  emit(b, hb_ir_emit(b, HB_IR_X87_FPREM1), dec);  return HB_OK;
        case HB_INS_X87_FPREM:   emit(b, hb_ir_emit(b, HB_IR_X87_FPREM), dec);   return HB_OK;
        case HB_INS_X87_FYL2XP1: emit(b, hb_ir_emit(b, HB_IR_X87_FYL2XP1), dec); return HB_OK;
        case HB_INS_X87_FSINCOS: emit(b, hb_ir_emit(b, HB_IR_X87_FSINCOS), dec); return HB_OK;
        case HB_INS_X87_FSCALE:  emit(b, hb_ir_emit(b, HB_IR_X87_FSCALE), dec);  return HB_OK;
        case HB_INS_X87_FSIN:    emit(b, hb_ir_emit(b, HB_IR_X87_FSIN), dec);    return HB_OK;
        case HB_INS_X87_FCOS:    emit(b, hb_ir_emit(b, HB_IR_X87_FCOS), dec);    return HB_OK;
        /* Environment + control — same as x86. */
        case HB_INS_X87_FXAM:    emit(b, hb_ir_emit(b, HB_IR_X87_FXAM), dec);    return HB_OK;
        case HB_INS_X87_FINCSTP: emit(b, hb_ir_emit(b, HB_IR_X87_FINCSTP), dec); return HB_OK;
        case HB_INS_X87_FDECSTP: emit(b, hb_ir_emit(b, HB_IR_X87_FDECSTP), dec); return HB_OK;
        case HB_INS_X87_FNSTCW:  emit(b, hb_ir_emit(b, HB_IR_X87_FNSTCW), dec);  return HB_OK;
        case HB_INS_X87_FIST:    emit(b, hb_ir_emit(b, HB_IR_X87_FIST), dec);    return HB_OK;
        /* D9 D0/E0/E1/E4 — same handlers as x86. */
        case HB_INS_X87_FNOP:    emit(b, hb_ir_emit(b, HB_IR_X87_FNOP), dec);    return HB_OK;
        case HB_INS_X87_FCHS:    emit(b, hb_ir_emit(b, HB_IR_X87_FCHS), dec);    return HB_OK;
        case HB_INS_X87_FABS:    emit(b, hb_ir_emit(b, HB_IR_X87_FABS), dec);    return HB_OK;
        case HB_INS_X87_FTST:    emit(b, hb_ir_emit(b, HB_IR_X87_FTST), dec);    return HB_OK;
        /* MISC safety net — same as x86. */
        case HB_INS_X87_MISC:
            emit(b, hb_ir_emit(b, HB_IR_NOP), dec);
            return HB_OK;
        case HB_INS_NOP: {
            emit(b, hb_ir_emit(b, HB_IR_NOP), dec);
            return HB_OK;
        }
        case HB_INS_CLD: {
            emit(b, hb_ir_emit(b, HB_IR_CLD), dec);
            return HB_OK;
        }
        case HB_INS_STD: {
            emit(b, hb_ir_emit(b, HB_IR_STD), dec);
            return HB_OK;
        }
        case HB_INS_FENCE: {
            emit(b, hb_ir_emit_fence(b, (hb_fence_kind_t)dec->op1.imm), dec);
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
