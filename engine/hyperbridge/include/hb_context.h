#ifndef HB_CONTEXT_H
#define HB_CONTEXT_H

#include "hb_result.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest architecture */
typedef enum {
    HB_ARCH_X64,
    HB_ARCH_X86
} hb_arch_t;

/* Execution backend */
typedef enum {
    HB_BACKEND_INTERP,
    HB_BACKEND_JIT,
    HB_BACKEND_AOT
} hb_backend_t;

/* Guest CPU mode */
typedef enum {
    HB_MODE_64BIT,
    HB_MODE_32BIT
} hb_mode_t;

/* Feature flags */
typedef struct {
    bool hyperbridge_enabled;
    int  mode; /* 0=off, 1=probe, 2=function, 3=module, 4=app */
    hb_arch_t arch;
    hb_backend_t backend;
    bool cache_enabled;
    bool trace_enabled;
    bool fallback_enabled;
} hb_config_t;

/* Guest register file x64 */
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi;
    uint64_t rsp, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    /* XMM state stores the low 128 bits of XMM/YMM registers. */
    uint64_t xmm[16][2];
} hb_regs_x64_t;

typedef struct {
    uint16_t control_word;
    uint16_t status_word;
    uint16_t tag_word;
    uint16_t reserved0;
    uint32_t top;
    uint32_t reserved1;
    double st[8];
} hb_x87_state_t;

/* Guest register file x86 */
typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi;
    uint32_t esp, ebp;
    uint32_t eip;
    uint32_t eflags;
    hb_x87_state_t x87;
    uint64_t xmm[8][2];
    /* Segment registers (i386-only — required by PUSH/POP ES/CS/SS/DS,
     * far CALL/JMP, RETF, IRET, etc.). Lane A only consumes eax..edi so
     * appending these at the END of the struct is safe (existing offsets
     * preserved). seg[0]=ES, seg[1]=CS, seg[2]=SS, seg[3]=DS, seg[4]=FS, seg[5]=GS. */
    uint16_t seg[6];
} hb_regs_x86_t;

/* Flags */
typedef struct {
    bool zf, sf, cf, of, pf, af;
} hb_flags_t;

typedef enum {
    HB_FLAG_BIT_ZF = 1u << 0,
    HB_FLAG_BIT_SF = 1u << 1,
    HB_FLAG_BIT_CF = 1u << 2,
    HB_FLAG_BIT_OF = 1u << 3,
    HB_FLAG_BIT_PF = 1u << 4,
    HB_FLAG_BIT_AF = 1u << 5,
    HB_FLAG_BIT_ALL = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                      HB_FLAG_BIT_OF | HB_FLAG_BIT_PF | HB_FLAG_BIT_AF
} hb_flag_bit_t;

typedef enum {
    HB_LAZY_FLAGS_NONE,
    HB_LAZY_FLAGS_ADD,
    HB_LAZY_FLAGS_ADC,
    HB_LAZY_FLAGS_SUB,
    HB_LAZY_FLAGS_SBB,
    HB_LAZY_FLAGS_AND,
    HB_LAZY_FLAGS_OR,
    HB_LAZY_FLAGS_XOR,
    HB_LAZY_FLAGS_SHL,
    HB_LAZY_FLAGS_SHR,
    HB_LAZY_FLAGS_SAR,
    HB_LAZY_FLAGS_CMP,
    HB_LAZY_FLAGS_TEST,
    HB_LAZY_FLAGS_UNKNOWN
} hb_lazy_flags_kind_t;

typedef struct {
    bool pending;
    hb_lazy_flags_kind_t kind;
    uint8_t width;
    uint64_t lhs;
    uint64_t rhs;
    uint64_t result;
    uint64_t count;
    uint32_t valid_mask;
    uint32_t unsupported_mask;
    uint32_t materialized_mask;
} hb_lazy_flags_t;

/* Guest virtual address */
typedef uint64_t hb_gva_t;

/* Forward declaration for AOT cache handle */
struct hb_cache_ext;

/* Runtime context */
typedef struct hb_context hb_context_t;

struct hb_context {
    hb_arch_t arch;
    hb_mode_t mode;
    hb_backend_t backend;
    hb_config_t config;

    union {
        hb_regs_x64_t x64;
        hb_regs_x86_t x86;
    } regs;

    hb_flags_t flags;
    hb_lazy_flags_t lazy_flags;

    /* Memory sandbox handle */
    struct hb_memory* memory;

    /* Step counter */
    uint64_t step_count;
    uint64_t step_limit;

    /* Block counter */
    uint64_t block_count;
    uint64_t block_limit;

    /* Guest program counter */
    hb_gva_t pc;

    /* Guest segment bases. Windows x64 uses GS for the TEB. */
    uint64_t fs_base;
    uint64_t gs_base;

    /* Guest segment selectors. i386 CONTEXT capture stores these with MOV Sreg. */
    uint16_t seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss;

    /* Exit status */
    int exit_code;
    hb_result_t last_result;

    /* Trace buffer */
    struct hb_trace* trace;

    /* Cache handle */
    struct hb_cache* cache;

    /* Thunk table */
    struct hb_thunk_table* thunks;

    /* AOT cache */
    uint64_t module_id;
    struct hb_cache_ext* aot_cache;

    /* User data */
    void* user_data;

    /* YMM high halves for interpreter AVX/AVX2 semantics. Appended to keep
       existing hb_context_t offsets stable for the ARM64 JIT lane. */
    uint64_t ymm_hi[16][2];

    /* ZMM bits 256..511 plus AVX-512 opmask state. Appended for interpreter
       EVEX semantics; existing XMM/YMM storage remains unchanged. */
    uint64_t zmm_hi[16][4];
    uint64_t k[8];
};

hb_context_t* hb_context_create(hb_arch_t arch, hb_backend_t backend);
void hb_context_destroy(hb_context_t* ctx);
hb_result_t hb_context_reset(hb_context_t* ctx);
hb_result_t hb_context_set_pc(hb_context_t* ctx, hb_gva_t pc);
hb_result_t hb_context_set_step_limit(hb_context_t* ctx, uint64_t limit);
hb_result_t hb_context_set_block_limit(hb_context_t* ctx, uint64_t limit);

#ifdef __cplusplus
}
#endif

#endif
