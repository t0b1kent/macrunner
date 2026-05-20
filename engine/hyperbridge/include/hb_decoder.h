#ifndef HB_DECODER_H
#define HB_DECODER_H

#include "hb_result.h"
#include "hb_context.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded instruction */
typedef struct {
    uint64_t addr;        /* guest address */
    uint8_t len;          /* instruction length in bytes */
    uint8_t bytes[15];    /* raw instruction bytes */

    /* Mnemonic / op */
    enum {
        HB_INS_MOV, HB_INS_LEA, HB_INS_ADD, HB_INS_ADC, HB_INS_SUB, HB_INS_SBB,
        HB_INS_AND, HB_INS_OR, HB_INS_XOR, HB_INS_NOT, HB_INS_NEG,
        HB_INS_INC, HB_INS_DEC, HB_INS_MUL, HB_INS_IMUL, HB_INS_DIV, HB_INS_IDIV,
        HB_INS_BT, HB_INS_BTS, HB_INS_BTR, HB_INS_BTC,
        HB_INS_SHL, HB_INS_SHR, HB_INS_SAR, HB_INS_ROL, HB_INS_ROR,
        HB_INS_CMP, HB_INS_TEST, HB_INS_CMPXCHG, HB_INS_XCHG, HB_INS_XADD, HB_INS_PUSH, HB_INS_POP,
        HB_INS_CALL, HB_INS_RET, HB_INS_JMP, HB_INS_Jcc,
        HB_INS_SETcc, HB_INS_CMOVcc,
        HB_INS_MOVZX, HB_INS_MOVSX, HB_INS_MOVSXD,
        HB_INS_CDQE, HB_INS_CWD, HB_INS_LEAVE, HB_INS_LAHF, HB_INS_SAHF, HB_INS_CPUID, HB_INS_XGETBV, HB_INS_NOP,
        HB_INS_SCAS, HB_INS_STOS,
        HB_INS_TZCNT, HB_INS_BSR, HB_INS_BSWAP, HB_INS_SSE_MOV,
        HB_INS_XMM_AND, HB_INS_XMM_ANDN, HB_INS_XMM_OR, HB_INS_XORPS, HB_INS_PXOR,
        HB_INS_PCMPEQB, HB_INS_PCMPEQW, HB_INS_PCMPEQD,
        HB_INS_PCMPGTB, HB_INS_PCMPGTW, HB_INS_PCMPGTD,
        HB_INS_PMOVMSKB, HB_INS_MOVMSKPS, HB_INS_MOVMSKPD,
        HB_INS_UNPCKLPS, HB_INS_UNPCKLPD, HB_INS_UNPCKHPS, HB_INS_UNPCKHPD,
        HB_INS_PUNPCKLBW, HB_INS_PUNPCKLWD, HB_INS_PUNPCKLDQ, HB_INS_PUNPCKLQDQ,
        HB_INS_PUNPCKHBW, HB_INS_PUNPCKHWD, HB_INS_PUNPCKHDQ, HB_INS_PUNPCKHQDQ,
        HB_INS_PSHUFD, HB_INS_PSHUFLW, HB_INS_PSHUFHW,
        HB_INS_PSRLW, HB_INS_PSRAW, HB_INS_PSLLW,
        HB_INS_PSRLD, HB_INS_PSRAD, HB_INS_PSLLD,
        HB_INS_PSRLQ, HB_INS_PSLLQ, HB_INS_PSRLDQ, HB_INS_PSLLDQ,
        HB_INS_MOVD, HB_INS_CVTDQ2PD, HB_INS_CVTDQ2PS, HB_INS_CVTPS2DQ, HB_INS_CVTTPS2DQ,
        HB_INS_CVTPS2PD, HB_INS_CVTPD2PS, HB_INS_CVTSS2SD, HB_INS_CVTSD2SS,
        HB_INS_CVTSI2SD, HB_INS_CVTSI2SS,
        HB_INS_ADDPS, HB_INS_ADDPD, HB_INS_ADDSS, HB_INS_ADDSD,
        HB_INS_SUBPS, HB_INS_SUBPD, HB_INS_SUBSS, HB_INS_SUBSD,
        HB_INS_DIVSD, HB_INS_MULSD,
        HB_INS_DIVSS, HB_INS_MULSS,
        HB_INS_MINPS, HB_INS_MAXPS, HB_INS_MINPD, HB_INS_MAXPD,
        HB_INS_MINSS, HB_INS_MAXSS, HB_INS_MINSD, HB_INS_MAXSD,
        HB_INS_COMISS, HB_INS_COMISD,
        HB_INS_CVTTSD2SI, HB_INS_CVTTSS2SI,
        HB_INS_PADDB, HB_INS_PADDW, HB_INS_PADDD, HB_INS_PADDQ,
        HB_INS_PSUBB, HB_INS_PSUBW, HB_INS_PSUBD, HB_INS_PSUBQ,
        HB_INS_UNKNOWN,
        HB_INS_UNSUPPORTED
    } opcode;

    /* Condition for SETcc/CMOVcc/Jcc */
    enum {
        HB_COND_NONE = 0,
        HB_COND_E, HB_COND_NE, HB_COND_S, HB_COND_NS,
        HB_COND_G, HB_COND_GE, HB_COND_L, HB_COND_LE,
        HB_COND_A, HB_COND_AE, HB_COND_B, HB_COND_BE,
        HB_COND_O, HB_COND_NO, HB_COND_P, HB_COND_NP,
        HB_COND_C, HB_COND_NC
    } cond;

    /* Operands */
    struct {
        bool present;
        bool is_reg;
        bool is_mem;
        bool is_imm;
        int reg;      /* register index or -1 */
        uint8_t reg_offset; /* byte offset for legacy AH/CH/DH/BH register aliases */
        int64_t imm;
        struct {
            int base;   /* -1 if none */
            int index;  /* -1 if none */
            uint8_t scale;
            int64_t disp;
            bool rip_relative;
            uint64_t rip_target; /* resolved RIP-relative */
            uint8_t segment;     /* legacy segment prefix: 0 none, 0x64 FS, 0x65 GS */
            bool addr32;          /* x86-64 address-size override (0x67): 32-bit effective address */
        } mem;
        uint8_t size; /* operand size in bytes */
    } op1, op2, op3;

    /* REX prefix info */
    bool has_rex;
    uint8_t rex_w;
    uint8_t rex_r;
    uint8_t rex_x;
    uint8_t rex_b;
    uint8_t segment_prefix; /* 0 none, 0x64 FS, 0x65 GS */
    bool address32_prefix;  /* 0x67 address-size override */

    /* ModRM/SIB info */
    bool has_modrm;
    uint8_t mod;
    uint8_t reg_op;
    uint8_t rm;
    bool has_sib;
    uint8_t sib_scale;
    uint8_t sib_index;
    uint8_t sib_base;

    /* Flags affected */
    bool writes_flags;
    bool reads_flags;

    /* Branch info */
    bool is_branch;
    bool is_call;
    bool is_ret;
    bool is_conditional;
    uint64_t branch_target; /* relative target resolved */
    uint8_t ret_imm;        /* ret N */

    /* Stack info */
    int stack_delta; /* bytes pushed (+) or popped (-) */

} hb_decoded_t;

/* Decoder state */
typedef struct {
    hb_arch_t arch;
    const uint8_t* code;
    size_t code_len;
    uint64_t base_addr;
    size_t pos;
} hb_decoder_t;

hb_decoder_t* hb_decoder_create(hb_arch_t arch, const uint8_t* code, size_t code_len, uint64_t base_addr);
void hb_decoder_destroy(hb_decoder_t* d);

hb_result_t hb_decode_next(hb_decoder_t* d, hb_decoded_t* out);
hb_result_t hb_decode_at(hb_decoder_t* d, size_t offset, hb_decoded_t* out);

/* x64 specific */
hb_result_t hb_decode_x64(const uint8_t* code, size_t len, uint64_t addr, hb_decoded_t* out);

/* x86 specific */
hb_result_t hb_decode_x86(const uint8_t* code, size_t len, uint64_t addr, hb_decoded_t* out);

/* Utility */
int hb_reg_index_from_modrm(uint8_t modrm, bool rex_r, bool is_64bit);
int hb_rm_index_from_modrm(uint8_t modrm, bool rex_b, bool is_64bit);
const char* hb_opcode_name(int opcode);
const char* hb_cond_name(int cond);

#ifdef __cplusplus
}
#endif

#endif
