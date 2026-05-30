#ifndef HB_IR_H
#define HB_IR_H

#include "hb_result.h"
#include "hb_context.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IR operand types */
typedef enum {
    HB_OP_REG,
    HB_OP_IMM,
    HB_OP_MEM,
    HB_OP_LABEL,
    HB_OP_NONE
} hb_op_type_t;

/* IR value size */
typedef enum {
    HB_SIZE_8 = 1,
    HB_SIZE_16 = 2,
    HB_SIZE_32 = 4,
    HB_SIZE_64 = 8,
    HB_SIZE_128 = 16
} hb_size_t;

/* x64 registers */
typedef enum {
    HB_REG_RAX = 0, HB_REG_RCX, HB_REG_RDX, HB_REG_RBX,
    HB_REG_RSP, HB_REG_RBP, HB_REG_RSI, HB_REG_RDI,
    HB_REG_R8, HB_REG_R9, HB_REG_R10, HB_REG_R11,
    HB_REG_R12, HB_REG_R13, HB_REG_R14, HB_REG_R15,
    HB_REG_RIP,
    HB_REG_XMM0, HB_REG_XMM1, HB_REG_XMM2, HB_REG_XMM3,
    HB_REG_XMM4, HB_REG_XMM5, HB_REG_XMM6, HB_REG_XMM7,
    HB_REG_XMM8, HB_REG_XMM9, HB_REG_XMM10, HB_REG_XMM11,
    HB_REG_XMM12, HB_REG_XMM13, HB_REG_XMM14, HB_REG_XMM15,
    HB_REG_COUNT
} hb_reg_t;

/* x86 registers */
typedef enum {
    HB_REG_X86_EAX = 0, HB_REG_X86_ECX, HB_REG_X86_EDX, HB_REG_X86_EBX,
    HB_REG_X86_ESP, HB_REG_X86_EBP, HB_REG_X86_ESI, HB_REG_X86_EDI,
    HB_REG_X86_EIP,
    HB_REG_X86_XMM0, HB_REG_X86_XMM1, HB_REG_X86_XMM2, HB_REG_X86_XMM3,
    HB_REG_X86_XMM4, HB_REG_X86_XMM5, HB_REG_X86_XMM6, HB_REG_X86_XMM7,
    HB_REG_X86_COUNT
} hb_reg_x86_t;

/* IR operand */
typedef struct {
    hb_op_type_t type;
    hb_size_t size;
    uint8_t reg_offset; /* x86-64 legacy high-8 regs: AH/CH/DH/BH live at byte offset 1. */
    union {
        hb_reg_t reg;
        int64_t imm;
        struct {
            hb_reg_t base;
            hb_reg_t index;
            uint8_t scale;
            int64_t disp;
            uint8_t segment; /* 0 none, 0x64 FS, 0x65 GS */
            bool addr32;     /* x86-64 0x67 address-size override: low-32 effective address */
        } mem;
        uint64_t label;
    };
} hb_ir_operand_t;

/* IR operation codes */
typedef enum {
    HB_IR_NOP,
    HB_IR_MOV,
    HB_IR_MOV_SEG,
    HB_IR_LEA,
    HB_IR_ADD,
    HB_IR_ADC,
    HB_IR_SUB,
    HB_IR_SBB,
    HB_IR_MUL,
    HB_IR_IMUL,
    HB_IR_DIV,
    HB_IR_IDIV,
    HB_IR_BT,
    HB_IR_BTS,
    HB_IR_BTR,
    HB_IR_BTC,
    HB_IR_AND,
    HB_IR_OR,
    HB_IR_XOR,
    HB_IR_NOT,
    HB_IR_NEG,
    HB_IR_SHL,
    HB_IR_SHR,
    HB_IR_SAR,
    HB_IR_ROL,      /* placeholder */
    HB_IR_ROR,      /* placeholder */
    HB_IR_SHLD,
    HB_IR_SHRD,
    HB_IR_CMP,
    HB_IR_TEST,
    HB_IR_CMPXCHG,
    HB_IR_CMPXCHG8B,
    HB_IR_XCHG,
    HB_IR_XADD,
    HB_IR_LAHF,
    HB_IR_SAHF,
    HB_IR_CPUID,
    HB_IR_XGETBV,
    HB_IR_SETcc,
    HB_IR_CMOVcc,
    HB_IR_LOAD,
    HB_IR_STORE,
    HB_IR_PUSH,
    HB_IR_POP,
    HB_IR_PUSHF,
    HB_IR_POPF,
    HB_IR_CALL,
    HB_IR_RET,
    HB_IR_JMP,
    HB_IR_Jcc,
    HB_IR_LOOP,
    HB_IR_JRCXZ,
    HB_IR_SIGN_EXTEND,
    HB_IR_CWD,
    HB_IR_MOVS,
    HB_IR_CMPS,
    HB_IR_LODS,
    HB_IR_SCAS,
    HB_IR_STOS,
    HB_IR_ZERO_EXTEND,
    HB_IR_TRUNC,
    HB_IR_TZCNT,
    HB_IR_LZCNT,
    HB_IR_BSR,
    HB_IR_BSWAP,
    HB_IR_XMM_AND,
    HB_IR_XMM_QWORD_LANE_MOV,
    HB_IR_XMM_ANDN,
    HB_IR_XMM_OR,
    HB_IR_XORPS,
    HB_IR_PCMPEQB,
    HB_IR_PCMPEQW,
    HB_IR_PCMPEQD,
    HB_IR_PCMPGTB,
    HB_IR_PCMPGTW,
    HB_IR_PCMPGTD,
    HB_IR_PMOVMSKB,
    HB_IR_MOVMSK,
    HB_IR_PUNPCK,
    HB_IR_PACKSSWB,
    HB_IR_PACKUSWB,
    HB_IR_PACKSSDW,
    HB_IR_PMULLW,
    HB_IR_PMULHW,
    HB_IR_PMULHUW,
    HB_IR_PMADDWD,
    HB_IR_PADDSB,
    HB_IR_PADDSW,
    HB_IR_PADDUSB,
    HB_IR_PADDUSW,
    HB_IR_PAVGB,
    HB_IR_PAVGW,
    HB_IR_PSHUFB,
    HB_IR_PINSRW,
    HB_IR_PEXTRW,
    HB_IR_PSHUF,
    HB_IR_FSHUF,
    HB_IR_PSRL,
    HB_IR_PSRA,
    HB_IR_PSLL,
    HB_IR_PSRLQ,
    HB_IR_PSLLQ,
    HB_IR_PSRLDQ,
    HB_IR_PSLLDQ,
    HB_IR_MOVD,
    HB_IR_CVTDQ2PD,
    HB_IR_CVTDQ2PS,
    HB_IR_CVTPS2DQ,
    HB_IR_CVTTPS2DQ,
    HB_IR_CVTPS2PD,
    HB_IR_CVTPD2PS,
    HB_IR_CVTSS2SD,
    HB_IR_CVTSD2SS,
    HB_IR_CVTSI2SD,
    HB_IR_CVTSI2SS,
    HB_IR_FSQRT,
    HB_IR_FRSQRT,
    HB_IR_FRCP,
    HB_IR_FADD,
    HB_IR_FSUB,
    HB_IR_FMUL,
    HB_IR_FDIV,
    HB_IR_ADDSD,
    HB_IR_SUBSD,
    HB_IR_DIVSD,
    HB_IR_MULSD,
    HB_IR_DIVSS,
    HB_IR_MULSS,
    HB_IR_FMIN,
    HB_IR_FMAX,
    HB_IR_COMISS,
    HB_IR_COMISD,
    HB_IR_CVTTSD2SI,
    HB_IR_CVTTSS2SI,
    HB_IR_PADD,
    HB_IR_PSUB,
    HB_IR_X87_FLD,
    HB_IR_X87_FST,
    HB_IR_X87_FSTP,
    HB_IR_X87_FILD,
    HB_IR_X87_FISTP,
    HB_IR_X87_FLDCW,
    HB_IR_X87_FNSTCW,
    HB_IR_X87_FNSTSW,
    HB_IR_X87_FADD,
    HB_IR_X87_FMUL,
    HB_IR_X87_FCOM,
    HB_IR_X87_FCOMP,
    HB_IR_X87_FSUB,
    HB_IR_X87_FSUBR,
    HB_IR_X87_FDIV,
    HB_IR_X87_FDIVR,
    HB_IR_X87_FADDP,
    HB_IR_X87_FMULP,
    HB_IR_X87_FCOMPP,
    HB_IR_X87_FSUBP,
    HB_IR_X87_FSUBRP,
    HB_IR_X87_FDIVP,
    HB_IR_X87_FDIVRP,
    HB_IR_X87_FXCH,
    HB_IR_X87_FRNDINT,
    HB_IR_X87_FNCLEX,
    HB_IR_X87_FNINIT,
    HB_IR_HOST_CALL,
    HB_IR_FAULT,
    HB_IR_UNSUPPORTED
} hb_ir_op_t;

/* Condition codes for SETcc, CMOVcc, Jcc */
typedef enum {
    HB_CC_E,   /* equal / zero */
    HB_CC_NE,  /* not equal */
    HB_CC_S,   /* sign */
    HB_CC_NS,  /* not sign */
    HB_CC_G,   /* greater (signed) */
    HB_CC_GE,  /* greater or equal (signed) */
    HB_CC_L,   /* less (signed) */
    HB_CC_LE,  /* less or equal (signed) */
    HB_CC_A,   /* above (unsigned) */
    HB_CC_AE,  /* above or equal (unsigned) */
    HB_CC_B,   /* below (unsigned) */
    HB_CC_BE,  /* below or equal (unsigned) */
    HB_CC_O,   /* overflow */
    HB_CC_NO,  /* not overflow */
    HB_CC_P,   /* parity */
    HB_CC_NP   /* not parity */
} hb_cc_t;

/* IR instruction */
typedef struct hb_ir_instr {
    hb_ir_op_t op;
    hb_cc_t cc;           /* for SETcc, CMOVcc, Jcc */
    hb_ir_operand_t dst;
    hb_ir_operand_t src1;
    hb_ir_operand_t src2;
    uint64_t guest_addr;  /* original guest address */
    uint8_t guest_len;    /* original instruction length */
    bool zero_upper;      /* XMM partial load zeroes bytes above the loaded scalar */
    uint64_t target;      /* branch target guest address */
    const char* comment;
} hb_ir_instr_t;

/* Basic block */
typedef struct hb_ir_block {
    uint64_t id;
    uint64_t guest_addr;  /* start guest address */
    hb_ir_instr_t* instrs;
    size_t instr_count;
    size_t instr_cap;
    struct hb_ir_block** succ;
    size_t succ_count;
    struct hb_ir_block** pred;
    size_t pred_count;
} hb_ir_block_t;

/* Control-flow graph */
typedef struct {
    hb_ir_block_t** blocks;
    size_t block_count;
    size_t block_cap;
    hb_ir_block_t* entry;
} hb_ir_cfg_t;

/* IR function / translation unit */
typedef struct {
    uint64_t guest_addr;
    size_t guest_len;
    hb_ir_cfg_t* cfg;
    hb_ir_instr_t** flat_instrs; /* optional flat view */
    size_t flat_count;
    bool has_unsupported;
    const char* unsupported_reason;
    bool truncated;
    const char* truncation_reason;
} hb_ir_func_t;

/* IR builder */
typedef struct {
    hb_ir_func_t* func;
    hb_ir_block_t* current_block;
} hb_ir_builder_t;

/* IR API */
hb_ir_func_t* hb_ir_func_create(uint64_t guest_addr, size_t guest_len);
void hb_ir_func_destroy(hb_ir_func_t* func);

hb_ir_block_t* hb_ir_block_create(uint64_t id, uint64_t guest_addr);
void hb_ir_block_destroy(hb_ir_block_t* block);

hb_ir_cfg_t* hb_ir_cfg_create(void);
void hb_ir_cfg_destroy(hb_ir_cfg_t* cfg);
void hb_ir_cfg_add_block(hb_ir_cfg_t* cfg, hb_ir_block_t* block);
void hb_ir_cfg_add_edge(hb_ir_cfg_t* cfg, hb_ir_block_t* from, hb_ir_block_t* to);

hb_ir_builder_t* hb_ir_builder_create(hb_ir_func_t* func);
void hb_ir_builder_destroy(hb_ir_builder_t* b);
void hb_ir_builder_set_block(hb_ir_builder_t* b, hb_ir_block_t* block);

hb_ir_instr_t* hb_ir_emit(hb_ir_builder_t* b, hb_ir_op_t op);
hb_ir_instr_t* hb_ir_emit_mov(hb_ir_builder_t* b, hb_ir_operand_t dst, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_lea(hb_ir_builder_t* b, hb_ir_operand_t dst, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_binop(hb_ir_builder_t* b, hb_ir_op_t op, hb_ir_operand_t dst, hb_ir_operand_t a, hb_ir_operand_t b_op);
hb_ir_instr_t* hb_ir_emit_unop(hb_ir_builder_t* b, hb_ir_op_t op, hb_ir_operand_t dst, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_load(hb_ir_builder_t* b, hb_ir_operand_t dst, hb_ir_operand_t addr);
hb_ir_instr_t* hb_ir_emit_store(hb_ir_builder_t* b, hb_ir_operand_t addr, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_call(hb_ir_builder_t* b, uint64_t target);
hb_ir_instr_t* hb_ir_emit_ret(hb_ir_builder_t* b);
hb_ir_instr_t* hb_ir_emit_jmp(hb_ir_builder_t* b, uint64_t target);
hb_ir_instr_t* hb_ir_emit_jcc(hb_ir_builder_t* b, hb_cc_t cc, uint64_t target);
hb_ir_instr_t* hb_ir_emit_push(hb_ir_builder_t* b, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_pop(hb_ir_builder_t* b, hb_ir_operand_t dst);
hb_ir_instr_t* hb_ir_emit_cmp(hb_ir_builder_t* b, hb_ir_operand_t a, hb_ir_operand_t b_op);
hb_ir_instr_t* hb_ir_emit_test(hb_ir_builder_t* b, hb_ir_operand_t a, hb_ir_operand_t b_op);
hb_ir_instr_t* hb_ir_emit_setcc(hb_ir_builder_t* b, hb_cc_t cc, hb_ir_operand_t dst);
hb_ir_instr_t* hb_ir_emit_cmovcc(hb_ir_builder_t* b, hb_cc_t cc, hb_ir_operand_t dst, hb_ir_operand_t src);
hb_ir_instr_t* hb_ir_emit_host_call(hb_ir_builder_t* b, uint32_t thunk_id);
hb_ir_instr_t* hb_ir_emit_fault(hb_ir_builder_t* b, hb_result_t reason, const char* msg);
hb_ir_instr_t* hb_ir_emit_unsupported(hb_ir_builder_t* b, const char* feature, uint64_t guest_addr, uint8_t* bytes, size_t len);

/* Operand helpers */
hb_ir_operand_t hb_ir_reg(hb_reg_t reg, hb_size_t size);
hb_ir_operand_t hb_ir_imm(int64_t val, hb_size_t size);
hb_ir_operand_t hb_ir_mem(hb_reg_t base, hb_reg_t index, uint8_t scale, int64_t disp, hb_size_t size);
hb_ir_operand_t hb_ir_mem_segment(hb_reg_t base, hb_reg_t index, uint8_t scale,
                                  int64_t disp, hb_size_t size, uint8_t segment);
hb_ir_operand_t hb_ir_label(uint64_t id);
hb_ir_operand_t hb_ir_none(void);

/* Serialization */
char* hb_ir_func_to_json(const hb_ir_func_t* func);
char* hb_ir_func_to_string(const hb_ir_func_t* func);
hb_result_t hb_ir_func_validate(const hb_ir_func_t* func);

/* Register name helpers */
const char* hb_reg_name(hb_reg_t reg);
const char* hb_reg_x86_name(hb_reg_x86_t reg);

#ifdef __cplusplus
}
#endif

#endif
