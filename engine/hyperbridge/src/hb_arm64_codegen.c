#include "hb_codegen.h"
#include "hb_runtime.h"
#include "hb_memory.h"
#include "hb_flags.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

_Static_assert(offsetof(hb_lazy_flags_t, kind) == offsetof(hb_lazy_flags_t, pending) + 4,
               "hb_lazy_flags_t pending/kind layout changed");
_Static_assert(offsetof(hb_lazy_flags_t, width) == offsetof(hb_lazy_flags_t, pending) + 8,
               "hb_lazy_flags_t pending/width layout changed");
_Static_assert(offsetof(hb_lazy_flags_t, lhs) == offsetof(hb_lazy_flags_t, pending) + 16,
               "hb_lazy_flags_t header padding layout changed");
_Static_assert(offsetof(hb_lazy_flags_t, unsupported_mask) == offsetof(hb_lazy_flags_t, valid_mask) + 4,
               "hb_lazy_flags_t valid/unsupported mask layout changed");
_Static_assert(offsetof(hb_lazy_flags_t, materialized_mask) == offsetof(hb_lazy_flags_t, valid_mask) + 8,
               "hb_lazy_flags_t materialized mask layout changed");

/* --- ARM64 instruction encoding helpers --- */
static void emit_u32(hb_codegen_buffer_t* buf, uint32_t insn) {
    uint8_t b[4] = { insn & 0xFF, (insn >> 8) & 0xFF, (insn >> 16) & 0xFF, (insn >> 24) & 0xFF };
    hb_codegen_buffer_append(buf, b, 4);
}

static void emit_nop(hb_codegen_buffer_t* buf)   { emit_u32(buf, 0xd503201f); }
static void emit_ret(hb_codegen_buffer_t* buf)   { emit_u32(buf, 0xd65f03c0); }

static void emit_b(hb_codegen_buffer_t* buf, int32_t off) {
    uint32_t imm26 = ((off / 4) & 0x03FFFFFF);
    emit_u32(buf, 0x14000000 | imm26);
}

static void emit_mov_reg(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* ORR Xd, XZR, Xn */
    emit_u32(buf, 0xaa0003e0 | (rn << 16) | rd);
}

static void emit_add_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x8b000000 | (rm << 16) | (rn << 5) | rd);
}

static void emit_add_reg_lsl(hb_codegen_buffer_t* buf, int rd, int rn, int rm, uint32_t shift) {
    emit_u32(buf, 0x8b000000 | (rm << 16) | ((shift & 0x3f) << 10) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_sub_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xcb000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_and_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x8a000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_ands_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xea000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_orr_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xaa000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_eor_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xca000000 | (rm << 16) | (rn << 5) | rd);
}

static void emit_add_imm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imm12) {
    emit_u32(buf, 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static void emit_sub_imm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imm12) {
    emit_u32(buf, 0xd1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static void emit_subs_imm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imm12) {
    emit_u32(buf, 0xf1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_cmp_reg(hb_codegen_buffer_t* buf, int rn, int rm) {
    /* SUBS XZR, Xn, Xm */
    emit_u32(buf, 0xeb00001f | (rm << 16) | (rn << 5));
}

static void emit_cmp_imm(hb_codegen_buffer_t* buf, int rn, uint32_t imm12) {
    /* SUBS XZR, Xn, #imm12 */
    emit_u32(buf, 0xf100001f | ((imm12 & 0xFFF) << 10) | (rn << 5));
}

static void __attribute__((unused)) emit_tst_reg(hb_codegen_buffer_t* buf, int rn, int rm) {
    /* ANDS XZR, Xn, Xm */
    emit_u32(buf, 0xea00001f | (rm << 16) | (rn << 5));
}

static void emit_dmb_ish(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xd5033bbf);
}

static void emit_dmb_ishld(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xd50339bf);
}

static void emit_dmb_ishst(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xd5033abf);
}

static void emit_guest_fence(hb_codegen_buffer_t* buf, hb_fence_kind_t kind) {
    switch (kind) {
        case HB_FENCE_ACQUIRE:
            emit_dmb_ishld(buf);
            break;
        case HB_FENCE_RELEASE:
            emit_dmb_ishst(buf);
            break;
        case HB_FENCE_FULL:
        default:
            emit_dmb_ish(buf);
            break;
    }
}

static void emit_ldr_x(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDR Xt, [Xn, #off]  — off must be multiple of 8 */
    uint32_t imm12 = (off / 8) & 0xFFF;
    emit_u32(buf, 0xf9400000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_ldr_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDR Wt, [Xn, #off]  — off must be multiple of 4 */
    uint32_t imm12 = (off / 4) & 0xFFF;
    emit_u32(buf, 0xb9400000 | (imm12 << 10) | (rn << 5) | rt);
}

/* Arch-aware GP register load: 32-bit for x86 (zero-extends in ARM64 reg), 64-bit for x64 */
static void emit_ldr_gpr(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    if (buf->arch == HB_ARCH_X86) emit_ldr_w(buf, rt, rn, off);
    else emit_ldr_x(buf, rt, rn, off);
}

static void emit_ldrb_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDRB Wt, [Xn, #off] */
    emit_u32(buf, 0x39400000 | ((off & 0xfff) << 10) | (rn << 5) | rt);
}

static void emit_ldrh_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDRH Wt, [Xn, #off] — off must be multiple of 2 */
    uint32_t imm12 = (off / 2) & 0xFFF;
    emit_u32(buf, 0x79400000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_ldar_to_reg(hb_codegen_buffer_t* buf, int rt, int rn, hb_size_t size) {
    /* MacRunner: x86 permits UNALIGNED ordinary loads, but ARM64 LDAR (load-acquire)
     * faults SIGBUS (BUS_ADRALN) on a non-naturally-aligned address. Mono's string
     * compare (`cmp [rdx+8], r12` with rdx 4-aligned) hit this and livelocked.
     * Emit a plain LDR (unaligned-safe) + DMB ISHLD instead: the barrier preserves
     * x86 TSO load-acquire ordering (orders this load before subsequent loads+stores)
     * while LDR handles any alignment. Applies to ALL TSO loads (cmp operand, ret,
     * stack, general) since they all route through here. */
    switch (size) {
        case HB_SIZE_8:  emit_u32(buf, 0x39400000 | (rn << 5) | rt); break; /* LDRB Wt, [Xn] */
        case HB_SIZE_16: emit_u32(buf, 0x79400000 | (rn << 5) | rt); break; /* LDRH Wt, [Xn] */
        case HB_SIZE_32: emit_u32(buf, 0xb9400000 | (rn << 5) | rt); break; /* LDR Wt, [Xn] */
        case HB_SIZE_64:
        default:         emit_u32(buf, 0xf9400000 | (rn << 5) | rt); break; /* LDR Xt, [Xn] */
    }
    emit_u32(buf, 0xd50339bf); /* DMB ISHLD — acquire ordering (loads-before vs loads+stores-after) */
}

static void emit_str_x(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STR Xt, [Xn, #off] */
    uint32_t imm12 = (off / 8) & 0xFFF;
    emit_u32(buf, 0xf9000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_str_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STR Wt, [Xn, #off] — off must be multiple of 4 */
    uint32_t imm12 = (off / 4) & 0xFFF;
    emit_u32(buf, 0xb9000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_stp_x(hb_codegen_buffer_t* buf, int rt, int rt2, int rn, uint32_t off) {
    /* STP Xt1, Xt2, [Xn, #off] — off must be multiple of 8 and fit imm7. */
    uint32_t imm7 = (off / 8) & 0x7f;
    emit_u32(buf, 0xa9000000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}

static void emit_ldp_x(hb_codegen_buffer_t* buf, int rt, int rt2, int rn, uint32_t off) {
    /* LDP Xt1, Xt2, [Xn, #off] — off must be multiple of 8 and fit imm7. */
    uint32_t imm7 = (off / 8) & 0x7f;
    emit_u32(buf, 0xa9400000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt);
}

static void emit_strb_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STRB Wt, [Xn, #off] */
    emit_u32(buf, 0x39000000 | ((off & 0xfff) << 10) | (rn << 5) | rt);
}

static void emit_strh_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STRH Wt, [Xn, #off] — off must be multiple of 2 */
    uint32_t imm12 = (off / 2) & 0xFFF;
    emit_u32(buf, 0x79000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_stlr_from_reg(hb_codegen_buffer_t* buf, int rt, int rn, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  emit_u32(buf, 0x089ffc00 | (rn << 5) | rt); break; /* STLRB Wt, [Xn] */
        case HB_SIZE_16: emit_u32(buf, 0x489ffc00 | (rn << 5) | rt); break; /* STLRH Wt, [Xn] */
        case HB_SIZE_32: emit_u32(buf, 0x889ffc00 | (rn << 5) | rt); break; /* STLR Wt, [Xn] */
        case HB_SIZE_64:
        default:         emit_u32(buf, 0xc89ffc00 | (rn << 5) | rt); break; /* STLR Xt, [Xn] */
    }
}

static void emit_mov_imm64(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    /* MOVZ + up to 3 MOVK */
    emit_u32(buf, 0xd2800000 | ((val & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2a00000 | (((val >> 16) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2c00000 | (((val >> 32) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2e00000 | (((val >> 48) & 0xFFFF) << 5) | rd);
}

static void emit_mov_imm64_compact(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    bool seeded = false;
    for (unsigned hw = 0; hw < 4; hw++) {
        uint32_t part = (uint32_t)((val >> (hw * 16)) & 0xffffu);
        if (!part) continue;
        if (!seeded) {
            emit_u32(buf, 0xd2800000 | (hw << 21) | (part << 5) | rd);
            seeded = true;
        } else {
            emit_u32(buf, 0xf2800000 | (hw << 21) | (part << 5) | rd);
        }
    }
    if (!seeded)
        emit_u32(buf, 0xd2800000 | rd);
}

static void emit_mov_imm_compact(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    emit_mov_imm64_compact(buf, rd, val);
}

static void emit_blr(hb_codegen_buffer_t* buf, int rn) {
    emit_u32(buf, 0xd63f0000 | (rn << 5));
}

static void emit_bcond(hb_codegen_buffer_t* buf, int cond, int32_t off) {
    uint32_t imm19 = ((off / 4) & 0x7FFFF);
    emit_u32(buf, 0x54000000 | (imm19 << 5) | (cond & 0xF));
}

static void emit_cset_w(hb_codegen_buffer_t* buf, int rd, int cond) {
    /* CSET Wd, cond == CSINC Wd, WZR, WZR, invert(cond). */
    emit_u32(buf, 0x1a9f07e0 | (((cond ^ 1) & 0xf) << 12) | (rd & 31));
}

static void patch_u32(hb_codegen_buffer_t* buf, size_t pos, uint32_t insn) {
    if (!buf || !buf->code || pos + 4 > buf->size) return;
    buf->code[pos] = (uint8_t)(insn & 0xff);
    buf->code[pos + 1] = (uint8_t)((insn >> 8) & 0xff);
    buf->code[pos + 2] = (uint8_t)((insn >> 16) & 0xff);
    buf->code[pos + 3] = (uint8_t)((insn >> 24) & 0xff);
}

static size_t emit_bcond_deferred(hb_codegen_buffer_t* buf, int cond) {
    size_t pos = buf->size;
    emit_bcond(buf, cond, 0);
    return pos;
}

static void patch_bcond(hb_codegen_buffer_t* buf, size_t pos, int cond, size_t target) {
    int32_t off = (int32_t)target - (int32_t)pos;
    uint32_t imm19 = ((off / 4) & 0x7ffff);
    patch_u32(buf, pos, 0x54000000 | (imm19 << 5) | (cond & 0xf));
}

static size_t emit_b_deferred(hb_codegen_buffer_t* buf) {
    size_t pos = buf->size;
    emit_b(buf, 0);
    return pos;
}

static void patch_b(hb_codegen_buffer_t* buf, size_t pos, size_t target) {
    int32_t off = (int32_t)target - (int32_t)pos;
    uint32_t imm26 = ((off / 4) & 0x03ffffff);
    patch_u32(buf, pos, 0x14000000 | imm26);
}

static void __attribute__((unused)) emit_lslv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_lsrv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02400 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_asrv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02800 | (rm << 16) | (rn << 5) | rd);
}

static void emit_rbit_x(hb_codegen_buffer_t* buf, int rd, int rn) {
    emit_u32(buf, 0xdac00000 | (rn << 5) | rd);
}

static void emit_clz_x(hb_codegen_buffer_t* buf, int rd, int rn) {
    emit_u32(buf, 0xdac01000 | (rn << 5) | rd);
}

static void emit_csel_x(hb_codegen_buffer_t* buf, int rd, int rn, int rm, int cond) {
    emit_u32(buf, 0x9a800000 | (rm << 16) | ((cond & 0xf) << 12) | (rn << 5) | rd);
}

static void emit_sbfm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imms) {
    emit_u32(buf, 0x93400000 | ((imms & 0x3f) << 10) | (rn << 5) | rd);
}

/* MacRunner 2026-06-23 (ABZU native-twin probe): trace the .data global window. */
#include <stdio.h>
static int macrunner_hb_codegendv_enabled(void) {
    static int cache = -1;
    int v = __atomic_load_n(&cache, __ATOMIC_RELAXED);
    if (v < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_DATADIVERGE");
        v = e && e[0] && e[0] != '0';
        __atomic_store_n(&cache, v, __ATOMIC_RELAXED);
    }
    return v;
}
static int macrunner_hb_codegendv_hit(uint64_t addr) {
    if (!macrunner_hb_codegendv_enabled()) return 0;
    return !(addr + 8 <= 0x142941000ULL || addr >= 0x142942000ULL);
}

static void emit_ubfm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imms) {
    emit_u32(buf, 0xd3400000 | ((imms & 0x3f) << 10) | (rn << 5) | rd);
}

/* x86-32: convert 32-bit guest EA in x21 → 64-bit host pointer.
 * Mask upper 32 bits (handle EA arithmetic carry) then add ctx->guest32_base. */
static void emit_x86_ea_to_host(hb_codegen_buffer_t* buf) {
    if (buf->arch != HB_ARCH_X86) return;
    emit_ubfm(buf, 21, 21, 31);  /* UBFM X21, X21, #0, #31 = UXTW (zero-extend W21) */
    emit_ldr_x(buf, 22, 19, (uint32_t)offsetof(hb_context_t, guest32_base));
    emit_add_reg(buf, 21, 21, 22);
}

static void __attribute__((unused)) emit_mvn(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* ORN Xd, XZR, Xn */
    emit_u32(buf, 0xaa2003e0 | (rn << 16) | rd);
}

static void __attribute__((unused)) emit_neg(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* SUB Xd, XZR, Xn */
    emit_u32(buf, 0xcb0003e0 | (rn << 16) | rd);
}

static void emit_rev_x(hb_codegen_buffer_t* buf, int rd, int rn) {
    emit_u32(buf, 0xdac00c00 | (rn << 5) | rd);
}

static void emit_rev_w(hb_codegen_buffer_t* buf, int rd, int rn) {
    emit_u32(buf, 0x5ac00800 | (rn << 5) | rd);
}

static bool is_gpr_reg_operand(const hb_ir_operand_t* op) {
    return op && op->type == HB_OP_REG && op->reg < HB_REG_XMM0;
}

static bool is_plain_gpr_reg_operand(const hb_ir_operand_t* op) {
    return is_gpr_reg_operand(op) && op->reg_offset == 0;
}

static bool is_xmm_reg_operand(const hb_ir_operand_t* op) {
    return op && op->type == HB_OP_REG &&
           op->reg >= HB_REG_XMM0 && op->reg <= HB_REG_XMM15 &&
           op->size == HB_SIZE_128;
}

static bool operand_is_none_or_unset(const hb_ir_operand_t* op) {
    return op && (op->type == HB_OP_NONE ||
                  (op->type == HB_OP_REG && op->size == 0 &&
                   op->reg == 0 && op->reg_offset == 0));
}

static bool jit_direct_mem_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_MEM");
    return val && val[0] && val[0] != '0';
}

static bool direct_mem_codegen_arch_enabled(hb_codegen_buffer_t* buf) {
    /* Direct host-pointer memory codegen is x64-only; i386 needs helper EA truncation. */
    return buf && buf->arch != HB_ARCH_X86;
}

static bool jit_direct_mem_codegen_enabled(hb_codegen_buffer_t* buf) {
    return direct_mem_codegen_arch_enabled(buf) && jit_direct_mem_enabled();
}

static bool jit_direct_scalar_scan_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN");
    if (jit_direct_mem_enabled()) return true;
    if (!val || !val[0]) return true;
    return val[0] != '0';
}

static bool jit_direct_stack_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_STACK");
    return val && val[0] && val[0] != '0';
}

/* Prologue: canonical Windows ARM64 packed-unwind layout for x19-x23, lr; x19 = ctx */
static void emit_prologue(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xa9bd53f3); /* STP X19, X20, [SP, #-48]! */
    emit_u32(buf, 0xa9015bf5); /* STP X21, X22, [SP, #16] */
    emit_u32(buf, 0xa9027bf7); /* STP X23, LR,  [SP, #32] */
    emit_mov_reg(buf, 19, 0); /* MOV X19, X0 (ctx) */
}

/* Epilogue: restore and ret */
static void emit_epilogue(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xa9427bf7); /* LDP X23, LR,  [SP, #32] */
    emit_u32(buf, 0xa9415bf5); /* LDP X21, X22, [SP, #16] */
    emit_u32(buf, 0xa8c353f3); /* LDP X19, X20, [SP], #48 */
    emit_ret(buf);
}

static void emit_return_if_helper_failed(hb_codegen_buffer_t* buf) {
    size_t ok_branch;
    emit_ldr_w(buf, 22, 19, (uint32_t)offsetof(hb_context_t, last_result));
    emit_cmp_imm(buf, 22, 0);
    ok_branch = emit_bcond_deferred(buf, 0); /* EQ -> skip inline epilogue */
    emit_epilogue(buf);
    patch_bcond(buf, ok_branch, 0, buf->size);
}

/* x64 register file offsets in hb_context_t */
static size_t x64_reg_off(int idx) {
    switch (idx) {
        case 0:  return offsetof(hb_context_t, regs.x64.rax);
        case 1:  return offsetof(hb_context_t, regs.x64.rcx);
        case 2:  return offsetof(hb_context_t, regs.x64.rdx);
        case 3:  return offsetof(hb_context_t, regs.x64.rbx);
        case 4:  return offsetof(hb_context_t, regs.x64.rsp);
        case 5:  return offsetof(hb_context_t, regs.x64.rbp);
        case 6:  return offsetof(hb_context_t, regs.x64.rsi);
        case 7:  return offsetof(hb_context_t, regs.x64.rdi);
        case 8:  return offsetof(hb_context_t, regs.x64.r8);
        case 9:  return offsetof(hb_context_t, regs.x64.r9);
        case 10: return offsetof(hb_context_t, regs.x64.r10);
        case 11: return offsetof(hb_context_t, regs.x64.r11);
        case 12: return offsetof(hb_context_t, regs.x64.r12);
        case 13: return offsetof(hb_context_t, regs.x64.r13);
        case 14: return offsetof(hb_context_t, regs.x64.r14);
        case 15: return offsetof(hb_context_t, regs.x64.r15);
        case 16: return offsetof(hb_context_t, regs.x64.rip);
        default: return 0;
    }
}

/* x86-32 register file offsets — Intel idx matches HB_REG_X86_* enum order */
static size_t x86_reg_off(int idx) {
    switch (idx) {
        case 0:  return offsetof(hb_context_t, regs.x86.eax);
        case 1:  return offsetof(hb_context_t, regs.x86.ecx);
        case 2:  return offsetof(hb_context_t, regs.x86.edx);
        case 3:  return offsetof(hb_context_t, regs.x86.ebx);
        case 4:  return offsetof(hb_context_t, regs.x86.esp);
        case 5:  return offsetof(hb_context_t, regs.x86.ebp);
        case 6:  return offsetof(hb_context_t, regs.x86.esi);
        case 7:  return offsetof(hb_context_t, regs.x86.edi);
        case 16: return offsetof(hb_context_t, regs.x86.eip);  /* HB_REG_RIP shared */
        default: return 0;
    }
}

/* Arch-aware dispatch; buf->arch defaults to HB_ARCH_X64 (=0) when zero-initialised */
static size_t reg_off(hb_codegen_buffer_t* buf, int idx) {
    return (buf->arch == HB_ARCH_X86) ? x86_reg_off(idx) : x64_reg_off(idx);
}

/* Map IR condition to ARM64 condition code */
static int __attribute__((unused)) arm64_cond(hb_cc_t cc) {
    switch (cc) {
        case HB_CC_E:  return 0;  /* EQ */
        case HB_CC_NE: return 1;  /* NE */
        case HB_CC_A:  return 8;  /* HI */
        case HB_CC_AE: return 2;  /* HS */
        case HB_CC_B:  return 3;  /* LO */
        case HB_CC_BE: return 9;  /* LS */
        case HB_CC_G:  return 12; /* GT */
        case HB_CC_GE: return 10; /* GE */
        case HB_CC_L:  return 11; /* LT */
        case HB_CC_LE: return 13; /* LE */
        case HB_CC_S:  return 4;  /* MI */
        case HB_CC_NS: return 5;  /* PL */
        case HB_CC_O:  return 6;  /* VS */
        case HB_CC_NO: return 7;  /* VC */
        default: return 0;
    }
}

/* Load IR operand into X20 (scratch) */
static void emit_load_operand(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->type == HB_OP_REG) {
        size_t off = reg_off(buf, op->reg);
        emit_ldr_gpr(buf, 20, 19, (uint32_t)off);
    } else if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 20, (uint64_t)op->imm);
    } else {
        emit_mov_imm64(buf, 20, 0);
    }
}

/* Store X20 into IR operand (must be register) */
static void emit_store_x20_to_gpr_sized(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op);

static void emit_store_operand(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->type == HB_OP_REG) {
        emit_store_x20_to_gpr_sized(buf, op);
    }
}

static void emit_call_helper(hb_codegen_buffer_t* buf, void* fn);
extern uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr);
extern void     hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                                 uint64_t dst_reg, uint64_t dst_size,
                                                 uint64_t dst_reg_offset);
extern void     hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr,
                                          uint64_t val, uint64_t size);

#ifdef __APPLE__
static void* hb_jit_live_host_ptr(uint64_t addr, size_t bytes, hb_perm_t perms);
#endif

static bool is_direct_user_mem_operand(const hb_ir_operand_t* op) {
    if (!op || op->type != HB_OP_MEM) return false;
    if (op->mem.segment != 0 || op->mem.addr32) return false;
    if (op->size != HB_SIZE_8 && op->size != HB_SIZE_16 &&
        op->size != HB_SIZE_32 && op->size != HB_SIZE_64) return false;
    if (op->mem.scale != 1 && op->mem.scale != 2 &&
        op->mem.scale != 4 && op->mem.scale != 8) return false;
    if (op->mem.base != HB_REG_COUNT && op->mem.base >= HB_REG_XMM0) return false;
    if (op->mem.index != HB_REG_COUNT && op->mem.index >= HB_REG_XMM0) return false;
    return true;
}

static bool is_absolute_mem64_operand(const hb_ir_operand_t* op) {
    return op && op->type == HB_OP_MEM && op->size == HB_SIZE_64 &&
           op->mem.segment == 0 && !op->mem.addr32 &&
           op->mem.base == HB_REG_COUNT && op->mem.index == HB_REG_COUNT &&
           op->mem.scale == 1;
}

/*
 * KUSER_SHARED_DATA lives at the canonical low Windows VA 0x7ffe0000, and the
 * Wine x64 syscall-dispatcher pointer sits in the slot right after it at
 * 0x7ffe1000 (8 bytes).  Neither can be host-mapped on macOS arm64: the low 2GB
 * range is covered by PAGEZERO (see ntdll virtual.c), so Wine relocates the live
 * KUSER page to WINE_USER_SHARED_DATA_ADDRESS and the interpreter translates the
 * canonical address in macrunner_hb_special_read().  The JIT direct-mem fast path
 * has no such translation: a baked LDR of e.g. 0x7ffe0308 (the SystemCall flag
 * read in every x64 syscall thunk via `test byte ptr [0x7ffe0308], 1`) would
 * fault.  So absolute (register-less) accesses into this range must drop out of
 * the direct-mem path and fall back to the helper/lazy emit, which routes through
 * special_read.  Only the constant low-address form is special-cased here, so
 * register-bearing and non-KUSER scalar memory stays direct (no runtime cost). */
#define HB_KUSER_SHARED_DATA_GVA 0x7ffe0000ULL
#define HB_KUSER_GUARD_END       0x7ffe1008ULL /* KUSER page + 8-byte dispatcher slot */

static bool mem_operand_is_kuser_absolute(const hb_ir_operand_t* op) {
    uint64_t addr, end;
    if (!op || op->type != HB_OP_MEM) return false;
    if (op->mem.base != HB_REG_COUNT || op->mem.index != HB_REG_COUNT) return false;
    if (op->mem.addr32 || op->mem.segment != 0) return false;
    addr = (uint64_t)op->mem.disp;
    end = addr + (op->size ? (uint64_t)op->size : 1ULL);
    if (end < addr) return false; /* overflow guard */
    return addr < HB_KUSER_GUARD_END && end > HB_KUSER_SHARED_DATA_GVA;
}

static bool is_direct_user_xmm_mem_operand(const hb_ir_operand_t* op) {
    if (!op || op->type != HB_OP_MEM || op->size != HB_SIZE_128) return false;
    if (op->mem.segment != 0 || op->mem.addr32) return false;
    if (op->mem.scale != 1 && op->mem.scale != 2 &&
        op->mem.scale != 4 && op->mem.scale != 8) return false;
    if (op->mem.base != HB_REG_COUNT && op->mem.base >= HB_REG_XMM0) return false;
    if (op->mem.index != HB_REG_COUNT && op->mem.index >= HB_REG_XMM0) return false;
    return true;
}

static bool direct_user_mem_allowed(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (mem_operand_is_kuser_absolute(op)) return false;
    return direct_mem_codegen_arch_enabled(buf) && is_direct_user_mem_operand(op);
}

static bool direct_user_xmm_mem_allowed(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (mem_operand_is_kuser_absolute(op)) return false;
    return direct_mem_codegen_arch_enabled(buf) && is_direct_user_xmm_mem_operand(op);
}

static uint32_t x64_xmm_reg_off(hb_reg_t reg) {
    return (uint32_t)(offsetof(hb_context_t, regs.x64.xmm) +
                      (size_t)(reg - HB_REG_XMM0) * sizeof(((hb_regs_x64_t*)0)->xmm[0]));
}

static uint32_t x86_xmm_reg_off(hb_reg_t reg) {
    return (uint32_t)(offsetof(hb_context_t, regs.x86.xmm) +
                      (size_t)(reg - HB_REG_XMM0) * sizeof(((hb_regs_x86_t*)0)->xmm[0]));
}

static uint32_t xmm_reg_off(hb_codegen_buffer_t* buf, hb_reg_t reg) {
    return (buf->arch == HB_ARCH_X86) ? x86_xmm_reg_off(reg) : x64_xmm_reg_off(reg);
}

static void emit_direct_mem_addr(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->mem.base < HB_REG_XMM0) {
        emit_ldr_gpr(buf, 21, 19, (uint32_t)reg_off(buf, op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        emit_ldr_gpr(buf, 22, 19, (uint32_t)reg_off(buf, op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, 22, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, 22, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, 22);
        }
    }
    emit_x86_ea_to_host(buf);
}

static void emit_direct_mem_addr_for_instr(hb_codegen_buffer_t* buf,
                                           const hb_ir_operand_t* op,
                                           const hb_ir_instr_t* instr) {
    if (op->mem.base == HB_REG_RIP) {
        emit_mov_imm64(buf, 21, instr ? instr->guest_addr + instr->guest_len : 0);
    } else if (op->mem.base < HB_REG_XMM0) {
        emit_ldr_gpr(buf, 21, 19, (uint32_t)reg_off(buf, op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        emit_ldr_gpr(buf, 22, 19, (uint32_t)reg_off(buf, op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, 22, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, 22, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, 22);
        }
    }
    emit_x86_ea_to_host(buf);
}

static bool direct_mem_addr_preserves_x22(const hb_ir_operand_t* op) {
    if (!op || op->type != HB_OP_MEM) return false;
    if (op->mem.index < HB_REG_XMM0) return false;
    return op->mem.disp == 0 || (op->mem.disp > 0 && op->mem.disp < 4096) ||
           (op->mem.disp < 0 && op->mem.disp > -4096);
}

static bool mem_operand_uses_reg(const hb_ir_operand_t* op, hb_reg_t reg) {
    return op && op->type == HB_OP_MEM && (op->mem.base == reg || op->mem.index == reg);
}

static bool same_plain_gpr_operand(const hb_ir_operand_t* a, const hb_ir_operand_t* b) {
    return is_plain_gpr_reg_operand(a) && is_plain_gpr_reg_operand(b) &&
           a->reg == b->reg && a->size == b->size;
}

static bool same_mem_operand(const hb_ir_operand_t* a, const hb_ir_operand_t* b) {
    return a && b && a->type == HB_OP_MEM && b->type == HB_OP_MEM &&
           a->size == b->size &&
           a->mem.base == b->mem.base &&
           a->mem.index == b->mem.index &&
           a->mem.scale == b->mem.scale &&
           a->mem.disp == b->mem.disp &&
           a->mem.segment == b->mem.segment &&
           a->mem.addr32 == b->mem.addr32;
}

static bool wide_self_base_load_needs_helper(const hb_ir_instr_t* instr) {
    if (!instr || instr->op != HB_IR_LOAD || !is_gpr_reg_operand(&instr->dst))
        return false;
    if (instr->src1.type != HB_OP_MEM || instr->src1.size == HB_SIZE_8)
        return false;
    return instr->src1.mem.base == instr->dst.reg &&
           instr->src1.mem.index == HB_REG_COUNT &&
           instr->src1.mem.disp == 0;
}

static bool adjacent_mem64_operands(hb_codegen_buffer_t* buf,
                                    const hb_ir_operand_t* a,
                                    const hb_ir_operand_t* b) {
    return a && b && a->type == HB_OP_MEM && b->type == HB_OP_MEM &&
           a->size == HB_SIZE_64 && b->size == HB_SIZE_64 &&
           direct_user_mem_allowed(buf, a) && direct_user_mem_allowed(buf, b) &&
           a->mem.base == b->mem.base &&
           a->mem.index == b->mem.index &&
           a->mem.scale == b->mem.scale &&
           a->mem.segment == b->mem.segment &&
           a->mem.addr32 == b->mem.addr32 &&
           b->mem.disp == a->mem.disp + 8;
}

static bool emit_direct_mem_addr_with_override(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                               hb_reg_t override_reg, int override_arm_reg) {
    if (!direct_user_mem_allowed(buf, op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;

    if (op->mem.base < HB_REG_XMM0) {
        if (op->mem.base == override_reg) emit_mov_reg(buf, 21, override_arm_reg);
        else emit_ldr_gpr(buf, 21, 19, (uint32_t)reg_off(buf, op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        if (op->mem.index == override_reg) emit_mov_reg(buf, 23, override_arm_reg);
        else emit_ldr_gpr(buf, 23, 19, (uint32_t)reg_off(buf, op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, 23, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, 23, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, 23);
        }
    }
    emit_x86_ea_to_host(buf);
    return true;
}

static bool emit_direct_mem_addr_with_override_scratch(hb_codegen_buffer_t* buf,
                                                       const hb_ir_operand_t* op,
                                                       hb_reg_t override_reg,
                                                       int override_arm_reg,
                                                       int scratch_arm_reg) {
    if (!direct_user_mem_allowed(buf, op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;

    if (op->mem.base < HB_REG_XMM0) {
        if (op->mem.base == override_reg) emit_mov_reg(buf, 21, override_arm_reg);
        else emit_ldr_gpr(buf, 21, 19, (uint32_t)reg_off(buf, op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        if (op->mem.index == override_reg) emit_mov_reg(buf, scratch_arm_reg, override_arm_reg);
        else emit_ldr_gpr(buf, scratch_arm_reg, 19, (uint32_t)reg_off(buf, op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, scratch_arm_reg, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, scratch_arm_reg, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, scratch_arm_reg);
        }
    }
    emit_x86_ea_to_host(buf);
    return true;
}

static void emit_mask_x_reg_to_size(hb_codegen_buffer_t* buf, int reg, int scratch, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:
            emit_mov_imm_compact(buf, scratch, 0xffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_16:
            emit_mov_imm_compact(buf, scratch, 0xffffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_32:
            emit_mov_imm64(buf, scratch, 0xffffffffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_64:
        default:
            break;
    }
}

static bool imm_fits_size(uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  return (value & ~0xffULL) == 0;
        case HB_SIZE_16: return (value & ~0xffffULL) == 0;
        case HB_SIZE_32: return (value & ~0xffffffffULL) == 0;
        case HB_SIZE_64:
        default: return true;
    }
}

static bool emit_load_gpr_sized_to_reg(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                       int arm_reg) {
    size_t off;
    if (!is_gpr_reg_operand(op)) return false;
    off = reg_off(buf, op->reg) + op->reg_offset;
    switch (op->size) {
        case HB_SIZE_8:  emit_ldrb_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_16: emit_ldrh_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_32: emit_ldr_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_64:
            if (op->reg_offset != 0) return false;
            emit_ldr_x(buf, arm_reg, 19, (uint32_t)off);
            return true;
        default:
            return false;
    }
}

static uint32_t lazy_valid_mask_for_kind(hb_lazy_flags_kind_t kind) {
    switch (kind) {
        case HB_LAZY_FLAGS_ADD:
        case HB_LAZY_FLAGS_SUB:
        case HB_LAZY_FLAGS_CMP:
            return HB_FLAG_BIT_ALL;
        case HB_LAZY_FLAGS_AND:
        case HB_LAZY_FLAGS_OR:
        case HB_LAZY_FLAGS_XOR:
        case HB_LAZY_FLAGS_TEST:
            return HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                   HB_FLAG_BIT_OF | HB_FLAG_BIT_PF;
        default:
            return 0;
    }
}

static bool lazy_kind_result_only(hb_lazy_flags_kind_t kind) {
    return kind == HB_LAZY_FLAGS_AND || kind == HB_LAZY_FLAGS_OR ||
           kind == HB_LAZY_FLAGS_XOR || kind == HB_LAZY_FLAGS_TEST;
}

static bool lazy_kind_for_scalar_op(hb_ir_op_t op, hb_lazy_flags_kind_t* out) {
    if (!out) return false;
    switch (op) {
        case HB_IR_ADD:  *out = HB_LAZY_FLAGS_ADD; return true;
        case HB_IR_SUB:  *out = HB_LAZY_FLAGS_SUB; return true;
        case HB_IR_AND:  *out = HB_LAZY_FLAGS_AND; return true;
        case HB_IR_OR:   *out = HB_LAZY_FLAGS_OR; return true;
        case HB_IR_XOR:  *out = HB_LAZY_FLAGS_XOR; return true;
        case HB_IR_CMP:  *out = HB_LAZY_FLAGS_CMP; return true;
        case HB_IR_TEST: *out = HB_LAZY_FLAGS_TEST; return true;
        default: return false;
    }
}

static void emit_note_lazy_header(hb_codegen_buffer_t* buf, int header_reg, int width_reg,
                                  hb_lazy_flags_kind_t kind, hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    uint64_t header = 1u | ((uint64_t)(uint32_t)kind << 32);
    emit_mov_imm_compact(buf, header_reg, header);
    emit_mov_imm_compact(buf, width_reg, (uint64_t)width);
    emit_stp_x(buf, header_reg, width_reg, 19,
               lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, pending));
}

static void emit_note_lazy_masks(hb_codegen_buffer_t* buf, int scratch_reg, uint32_t valid) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    uint32_t unsupported = HB_FLAG_BIT_ALL & ~valid;
    uint64_t masks = ((uint64_t)unsupported << 32) | (uint64_t)valid;
    emit_mov_imm_compact(buf, scratch_reg, masks);
    emit_str_x(buf, scratch_reg, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, valid_mask));
    emit_str_w(buf, 31, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, materialized_mask));
}

static void emit_note_lazy_from_x20_x21_x22(hb_codegen_buffer_t* buf,
                                            hb_lazy_flags_kind_t kind,
                                            hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    uint32_t valid = lazy_valid_mask_for_kind(kind);
    if (!lazy_kind_result_only(kind))
        emit_stp_x(buf, 20, 21, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, lhs));
    emit_stp_x(buf, 22, 31, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, result));
    emit_note_lazy_header(buf, 16, 17, kind, width);
    emit_note_lazy_masks(buf, 16, valid);
}

static void emit_note_lazy_cmp_from_x23_x22_x21(hb_codegen_buffer_t* buf, hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    emit_stp_x(buf, 23, 22, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, lhs));
    emit_stp_x(buf, 21, 31, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, result));
    emit_note_lazy_header(buf, 16, 17, HB_LAZY_FLAGS_CMP, width);
    emit_note_lazy_masks(buf, 16, HB_FLAG_BIT_ALL);
}

static void emit_direct_mem_load_to_x20_base(hb_codegen_buffer_t* buf, hb_size_t size, int rn) {
    emit_ldar_to_reg(buf, 20, rn, size);
}

static void emit_direct_mem_load_to_x20(hb_codegen_buffer_t* buf, hb_size_t size) {
    emit_direct_mem_load_to_x20_base(buf, size, 21);
}

static void emit_direct_mem_store_from_x20_base(hb_codegen_buffer_t* buf, hb_size_t size, int rn) {
    emit_stlr_from_reg(buf, 20, rn, size);
}

static void emit_direct_mem_store_from_x20(hb_codegen_buffer_t* buf, hb_size_t size) {
    emit_direct_mem_store_from_x20_base(buf, size, 21);
}

/* LDAR/STLR do not have unsigned-offset forms. Keep X21 as the canonical base
 * for fusions that reuse it, and use X22 as a dead scratch address. */
static int emit_direct_mem_base_for_offset(hb_codegen_buffer_t* buf, uint32_t off) {
    if (!off) return 21;
    if (off < 4096) {
        emit_add_imm(buf, 22, 21, off);
    } else {
        emit_mov_imm64(buf, 22, off);
        emit_add_reg(buf, 22, 21, 22);
    }
    return 22;
}

static void emit_direct_mem_load_to_x20_off(hb_codegen_buffer_t* buf, hb_size_t size, uint32_t off) {
    emit_direct_mem_load_to_x20_base(buf, size, emit_direct_mem_base_for_offset(buf, off));
}

static void emit_direct_mem_store_from_x20_off(hb_codegen_buffer_t* buf, hb_size_t size, uint32_t off) {
    emit_direct_mem_store_from_x20_base(buf, size, emit_direct_mem_base_for_offset(buf, off));
}

static void emit_direct_mem_store_zero_off(hb_codegen_buffer_t* buf, hb_size_t size, uint32_t off) {
    emit_stlr_from_reg(buf, 31, emit_direct_mem_base_for_offset(buf, off), size);
}

static uint64_t direct_mem_alignment_mask(hb_size_t size) {
    switch (size) {
        case HB_SIZE_16: return 1u;
        case HB_SIZE_32: return 3u;
        case HB_SIZE_64: return 7u;
        case HB_SIZE_8:
        default: return 0u;
    }
}

static bool emit_direct_mem_load_to_gpr_tso(hb_codegen_buffer_t* buf,
                                            const hb_ir_operand_t* src,
                                            const hb_ir_operand_t* dst) {
    uint64_t mask;
    size_t aligned_branch = 0;
    size_t done_branch = 0;

    if (!buf || !src || !dst || !is_gpr_reg_operand(dst) ||
        !direct_user_mem_allowed(buf, src))
        return false;

    emit_direct_mem_addr(buf, src);
    mask = direct_mem_alignment_mask(src->size);
    if (mask) {
        emit_mov_imm_compact(buf, 22, mask);
        emit_ands_reg(buf, 22, 21, 22);
        aligned_branch = emit_bcond_deferred(buf, 0); /* EQ: runtime-aligned */

        emit_mov_reg(buf, 0, 19);
        emit_mov_reg(buf, 1, 21);
        emit_mov_imm_compact(buf, 2, (uint64_t)dst->reg);
        emit_mov_imm_compact(buf, 3, (uint64_t)dst->size);
        emit_mov_imm_compact(buf, 4, (uint64_t)dst->reg_offset);
        emit_call_helper(buf, (void*)hb_jit_helper_load_to_reg_sized);
        emit_return_if_helper_failed(buf);
        done_branch = emit_b_deferred(buf);

        patch_bcond(buf, aligned_branch, 0, buf->size);
    }

    emit_direct_mem_load_to_x20(buf, src->size);
    emit_store_x20_to_gpr_sized(buf, dst);
    if (done_branch)
        patch_b(buf, done_branch, buf->size);
    return true;
}

static bool emit_direct_mem_store_from_x20_tso(hb_codegen_buffer_t* buf,
                                               const hb_ir_operand_t* dst) {
    uint64_t mask;
    size_t aligned_branch = 0;
    size_t done_branch = 0;

    if (!buf || !dst || !direct_user_mem_allowed(buf, dst))
        return false;

    emit_direct_mem_addr(buf, dst);
    mask = direct_mem_alignment_mask(dst->size);
    if (mask) {
        emit_mov_imm_compact(buf, 22, mask);
        emit_ands_reg(buf, 22, 21, 22);
        aligned_branch = emit_bcond_deferred(buf, 0); /* EQ: runtime-aligned */

        emit_mov_reg(buf, 0, 19);
        emit_mov_reg(buf, 1, 21);
        emit_mov_reg(buf, 2, 20);
        emit_mov_imm_compact(buf, 3, (uint64_t)dst->size);
        emit_call_helper(buf, (void*)hb_jit_helper_store_sized);
        emit_return_if_helper_failed(buf);
        done_branch = emit_b_deferred(buf);

        patch_bcond(buf, aligned_branch, 0, buf->size);
    }

    emit_direct_mem_store_from_x20(buf, dst->size);
    if (done_branch)
        patch_b(buf, done_branch, buf->size);
    return true;
}

static bool direct_mem_unsigned_offset(hb_codegen_buffer_t* buf,
                                       const hb_ir_operand_t* op,
                                       uint32_t* off) {
    uint64_t disp;
    if (!direct_user_mem_allowed(buf, op) || !off) return false;
    if (op->mem.index != HB_REG_COUNT) return false;
    if (op->mem.base == HB_REG_COUNT || op->mem.base == HB_REG_RIP || op->mem.base >= HB_REG_XMM0)
        return false;
    if (op->mem.disp < 0) return false;
    disp = (uint64_t)op->mem.disp;
    switch (op->size) {
        case HB_SIZE_8:
            if (disp >= 4096) return false;
            break;
        case HB_SIZE_16:
            if (disp >= 8192 || (disp & 1)) return false;
            break;
        case HB_SIZE_32:
            if (disp >= 16384 || (disp & 3)) return false;
            break;
        case HB_SIZE_64:
            if (disp >= 32768 || (disp & 7)) return false;
            break;
        default:
            return false;
    }
    *off = (uint32_t)disp;
    return true;
}

static uint32_t emit_direct_mem_addr_with_offset(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    uint32_t off = 0;
    if (direct_mem_unsigned_offset(buf, op, &off)) {
        emit_ldr_gpr(buf, 21, 19, (uint32_t)reg_off(buf, op->mem.base));
        emit_x86_ea_to_host(buf);
        return off;
    }
    emit_direct_mem_addr(buf, op);
    return 0;
}

static bool adjacent_mem64_pair_offset(hb_codegen_buffer_t* buf,
                                       const hb_ir_operand_t* op,
                                       uint32_t* off) {
    uint32_t folded = 0;
    if (!direct_mem_unsigned_offset(buf, op, &folded)) return false;
    if (folded >= 512) return false;
    if (off) *off = folded;
    return true;
}

static bool stack_store_reg64_at(const hb_ir_instr_t* instr, int64_t disp) {
    return instr && instr->op == HB_IR_STORE &&
           instr->src1.type == HB_OP_MEM && instr->src1.size == HB_SIZE_64 &&
           instr->src1.mem.base == HB_REG_RSP && instr->src1.mem.index == HB_REG_COUNT &&
           instr->src1.mem.scale == 1 && instr->src1.mem.disp == disp &&
           instr->src1.mem.segment == 0 && !instr->src1.mem.addr32 &&
           instr->src2.type == HB_OP_REG && instr->src2.size == HB_SIZE_64;
}

static bool stack_load_reg64(const hb_ir_instr_t* instr, hb_reg_t* dst, uint32_t* disp) {
    int64_t d;
    if (!instr || (instr->op != HB_IR_LOAD && instr->op != HB_IR_MOV) ||
        !is_plain_gpr_reg_operand(&instr->dst) || instr->dst.size != HB_SIZE_64 ||
        instr->src1.type != HB_OP_MEM || instr->src1.size != HB_SIZE_64 ||
        instr->src1.mem.base != HB_REG_RSP || instr->src1.mem.index != HB_REG_COUNT ||
        instr->src1.mem.scale != 1 || instr->src1.mem.segment != 0 ||
        instr->src1.mem.addr32)
        return false;
    d = instr->src1.mem.disp;
    if (d < 0 || d > 32760 || (d & 7)) return false;
    if (dst) *dst = instr->dst.reg;
    if (disp) *disp = (uint32_t)d;
    return true;
}

static void emit_load_xmm_to_pair(hb_codegen_buffer_t* buf, hb_reg_t reg, int lo, int hi) {
    uint32_t off = xmm_reg_off(buf, reg);
    emit_ldr_x(buf, lo, 19, off);
    emit_ldr_x(buf, hi, 19, off + 8);
}

static void emit_load_xmm_to_x20_x22(hb_codegen_buffer_t* buf, hb_reg_t reg) {
    emit_load_xmm_to_pair(buf, reg, 20, 22);
}

static void emit_store_x20_x22_to_xmm(hb_codegen_buffer_t* buf, hb_reg_t reg) {
    uint32_t off = xmm_reg_off(buf, reg);
    emit_str_x(buf, 20, 19, off);
    emit_str_x(buf, 22, 19, off + 8);
}

static void emit_direct_mem128_load_to_x20_x22(hb_codegen_buffer_t* buf) {
    emit_ldr_x(buf, 20, 21, 0);
    emit_ldr_x(buf, 22, 21, 8);
    emit_dmb_ishld(buf);
}

static void emit_direct_mem128_store_from_x20_x22(hb_codegen_buffer_t* buf) {
    emit_dmb_ishst(buf);
    emit_str_x(buf, 20, 21, 0);
    emit_str_x(buf, 22, 21, 8);
}

static bool emit_load_xmm_operand_to_pair(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                          int lo, int hi) {
    if (is_xmm_reg_operand(op)) {
        emit_load_xmm_to_pair(buf, op->reg, lo, hi);
        return true;
    }
    if (jit_direct_mem_codegen_enabled(buf) && direct_user_xmm_mem_allowed(buf, op)) {
        emit_direct_mem_addr(buf, op);
        if (lo == 21) {
            emit_ldr_x(buf, hi, 21, 8);
            emit_ldr_x(buf, lo, 21, 0);
        } else {
            emit_ldr_x(buf, lo, 21, 0);
            emit_ldr_x(buf, hi, 21, 8);
        }
        emit_dmb_ishld(buf);
        return true;
    }
    return false;
}

static bool emit_load_gpr_sized_to_x20(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    return emit_load_gpr_sized_to_reg(buf, op, 20);
}

static void emit_store_x20_to_gpr_sized(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    size_t off = reg_off(buf, op->reg) + op->reg_offset;
    switch (op->size) {
        case HB_SIZE_8:
            emit_strb_w(buf, 20, 19, (uint32_t)off);
            break;
        case HB_SIZE_16:
            emit_strh_w(buf, 20, 19, (uint32_t)off);
            break;
        case HB_SIZE_32:
            if (buf->arch == HB_ARCH_X86) {
                emit_str_w(buf, 20, 19, (uint32_t)reg_off(buf, op->reg));
            } else {
                emit_ubfm(buf, 20, 20, 31);
                emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, op->reg));
            }
            break;
        case HB_SIZE_64:
        default:
            emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, op->reg));
            break;
    }
}

static void emit_store_zero_to_gpr_sized(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    size_t off = reg_off(buf, op->reg) + op->reg_offset;
    switch (op->size) {
        case HB_SIZE_8:
            emit_strb_w(buf, 31, 19, (uint32_t)off);
            break;
        case HB_SIZE_16:
            emit_strh_w(buf, 31, 19, (uint32_t)off);
            break;
        case HB_SIZE_32:
            if (buf->arch == HB_ARCH_X86)
                emit_str_w(buf, 31, 19, (uint32_t)reg_off(buf, op->reg));
            else
                emit_str_x(buf, 31, 19, (uint32_t)reg_off(buf, op->reg));
            break;
        case HB_SIZE_64:
        default:
            emit_str_x(buf, 31, 19, (uint32_t)reg_off(buf, op->reg));
            break;
    }
}

static bool operand_is_xmm_or_vecmem(const hb_ir_operand_t* op);
static void emit_set_pc_imm64(hb_codegen_buffer_t* buf, uint64_t pc);

static bool emit_native_scalar_mov(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (!instr || instr->op != HB_IR_MOV) return false;
    if (operand_is_xmm_or_vecmem(&instr->dst) || operand_is_xmm_or_vecmem(&instr->src1))
        return false;

    if (is_gpr_reg_operand(&instr->dst)) {
        if (instr->src1.type == HB_OP_REG) {
            if (!is_gpr_reg_operand(&instr->src1)) return false;
            if (!emit_load_gpr_sized_to_x20(buf, &instr->src1)) return false;
        } else if (instr->src1.type == HB_OP_IMM) {
            uint64_t imm = (uint64_t)instr->src1.imm;
            emit_mov_imm_compact(buf, 20, imm);
            if (!imm_fits_size(imm, instr->dst.size))
                emit_mask_x_reg_to_size(buf, 20, 23, instr->dst.size);
        } else if (jit_direct_mem_codegen_enabled(buf) &&
                   direct_user_mem_allowed(buf, &instr->src1)) {
            uint32_t off = emit_direct_mem_addr_with_offset(buf, &instr->src1);
            emit_direct_mem_load_to_x20_off(buf, instr->src1.size, off);
        } else {
            return false;
        }
        emit_store_x20_to_gpr_sized(buf, &instr->dst);
        return true;
    }

    if (jit_direct_mem_codegen_enabled(buf) && direct_user_mem_allowed(buf, &instr->dst) &&
        (instr->src1.type == HB_OP_REG || instr->src1.type == HB_OP_IMM)) {
        if (instr->src1.type == HB_OP_REG) {
            if (!is_gpr_reg_operand(&instr->src1)) return false;
            if (!emit_load_gpr_sized_to_x20(buf, &instr->src1)) return false;
        } else {
            emit_mov_imm_compact(buf, 20, (uint64_t)instr->src1.imm);
        }
        /* Use the alignment-checked TSO store: STLR (and LDAR) require natural alignment, but x86
         * permits unaligned 8-byte stores; the raw-STLR offset path SIGBUSes on an unaligned dest.
         * The _tso path runtime-checks alignment and falls back to the lazy helper when unaligned. */
        if (!emit_direct_mem_store_from_x20_tso(buf, &instr->dst)) return false;
        return true;
    }

    return false;
}

static bool emit_native_xmm_mov(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (!instr || instr->op != HB_IR_MOV) return false;
    if (is_xmm_reg_operand(&instr->dst) && is_xmm_reg_operand(&instr->src1)) {
        emit_load_xmm_to_x20_x22(buf, instr->src1.reg);
        emit_store_x20_x22_to_xmm(buf, instr->dst.reg);
        return true;
    }
    if (is_xmm_reg_operand(&instr->dst) && jit_direct_mem_codegen_enabled(buf) &&
        direct_user_xmm_mem_allowed(buf, &instr->src1)) {
        emit_direct_mem_addr(buf, &instr->src1);
        emit_direct_mem128_load_to_x20_x22(buf);
        emit_store_x20_x22_to_xmm(buf, instr->dst.reg);
        return true;
    }
    if (jit_direct_mem_codegen_enabled(buf) && direct_user_xmm_mem_allowed(buf, &instr->dst) &&
        is_xmm_reg_operand(&instr->src1)) {
        emit_load_xmm_to_x20_x22(buf, instr->src1.reg);
        /* emit_direct_mem_addr clobbers x22 for indexed / large-disp addresses (it loads the
         * index into x22); x22 holds the HIGH 64 bits of the XMM value to be stored.  Preserve
         * it across the address computation (mirrors emit_xmm_load_store_pair).  Without this,
         * a non-paired SSE store with an indexed dest corrupts the high qword. */
        if (direct_mem_addr_preserves_x22(&instr->dst)) {
            emit_direct_mem_addr(buf, &instr->dst);
        } else {
            emit_mov_reg(buf, 23, 22);
            emit_direct_mem_addr(buf, &instr->dst);
            emit_mov_reg(buf, 22, 23);
        }
        emit_direct_mem128_store_from_x20_x22(buf);
        return true;
    }
    return false;
}

static bool emit_native_xmm_logic(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (!instr || (instr->op != HB_IR_XMM_AND && instr->op != HB_IR_XMM_ANDN &&
                   instr->op != HB_IR_XMM_OR && instr->op != HB_IR_XORPS))
        return false;
    if (!is_xmm_reg_operand(&instr->dst))
        return false;
    if (!emit_load_xmm_operand_to_pair(buf, &instr->src1, 20, 22))
        return false;
    if (!emit_load_xmm_operand_to_pair(buf, &instr->src2, 21, 23))
        return false;

    switch (instr->op) {
        case HB_IR_XMM_AND:
            emit_and_reg(buf, 20, 20, 21);
            emit_and_reg(buf, 22, 22, 23);
            break;
        case HB_IR_XMM_ANDN:
            emit_mvn(buf, 20, 20);
            emit_mvn(buf, 22, 22);
            emit_and_reg(buf, 20, 20, 21);
            emit_and_reg(buf, 22, 22, 23);
            break;
        case HB_IR_XMM_OR:
            emit_orr_reg(buf, 20, 20, 21);
            emit_orr_reg(buf, 22, 22, 23);
            break;
        case HB_IR_XORPS:
            emit_eor_reg(buf, 20, 20, 21);
            emit_eor_reg(buf, 22, 22, 23);
            break;
        default:
            return false;
    }
    emit_store_x20_x22_to_xmm(buf, instr->dst.reg);
    return true;
}

static void emit_native_stack_push_x20(hb_codegen_buffer_t* buf) {
    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_sub_imm(buf, 21, 21, 8);
    emit_stlr_from_reg(buf, 20, 21, HB_SIZE_64);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
}

static bool emit_native_push(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (buf->arch == HB_ARCH_X86) return false;  /* i386: direct-stack breaks the prologue (DIRECT_STACK=0 default for x86) */
    if (!jit_direct_stack_enabled() || !instr || instr->op != HB_IR_PUSH) return false;
    if (instr->src1.type == HB_OP_REG) {
        if (!is_plain_gpr_reg_operand(&instr->src1) || instr->src1.size != HB_SIZE_64)
            return false;
        if (!emit_load_gpr_sized_to_x20(buf, &instr->src1)) return false;
    } else if (instr->src1.type == HB_OP_IMM) {
        emit_mov_imm64(buf, 20, (uint64_t)instr->src1.imm);
    } else {
        return false;
    }
    emit_native_stack_push_x20(buf);
    return true;
}

static bool emit_native_pop(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (buf->arch == HB_ARCH_X86) return false;  /* i386: direct-stack breaks the prologue (DIRECT_STACK=0 default for x86) */
    if (!jit_direct_stack_enabled() || !instr || instr->op != HB_IR_POP) return false;
    if (!is_plain_gpr_reg_operand(&instr->dst) || instr->dst.size != HB_SIZE_64)
        return false;
    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_ldar_to_reg(buf, 20, 21, HB_SIZE_64);
    emit_add_imm(buf, 21, 21, 8);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_store_x20_to_gpr_sized(buf, &instr->dst);
    return true;
}

static bool emit_native_ret(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    uint64_t adjust = 0;
    if (buf->arch == HB_ARCH_X86) return false;
    if (!jit_direct_stack_enabled() || !instr || instr->op != HB_IR_RET) return false;
    if (instr->src1.type == HB_OP_IMM) {
        adjust = (uint64_t)instr->src1.imm;
        if (adjust > 4095) return false;
    } else if (!operand_is_none_or_unset(&instr->src1)) {
        return false;
    }
    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_ldar_to_reg(buf, 20, 21, HB_SIZE_64);
    emit_add_imm(buf, 21, 21, 8);
    if (adjust) emit_add_imm(buf, 21, 21, (uint32_t)adjust);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static bool emit_native_direct_call(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (buf->arch == HB_ARCH_X86) return false;
    if (!jit_direct_stack_enabled() || !instr || instr->op != HB_IR_CALL) return false;
    if (instr->src1.type != HB_OP_NONE) return false;
    emit_mov_imm_compact(buf, 20, instr->guest_addr + instr->guest_len);
    emit_native_stack_push_x20(buf);
    emit_set_pc_imm64(buf, instr->target);
    return true;
}

static bool emit_native_branch_target_to_x20(hb_codegen_buffer_t* buf,
                                             const hb_ir_operand_t* op) {
    if (is_plain_gpr_reg_operand(op) && op->size == HB_SIZE_64) {
        emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, op->reg));
        return true;
    }
    if (jit_direct_mem_codegen_enabled(buf) && direct_user_mem_allowed(buf, op) &&
        op->size == HB_SIZE_64) {
        emit_direct_mem_addr(buf, op);
        emit_direct_mem_load_to_x20(buf, op->size);
        return true;
    }
    if (is_absolute_mem64_operand(op)) {
        emit_mov_reg(buf, 0, 19);
        emit_mov_imm64(buf, 1, (uint64_t)op->mem.disp);
        emit_call_helper(buf, (void*)hb_jit_helper_load_u64);
        emit_return_if_helper_failed(buf);
        emit_mov_reg(buf, 20, 0);
        return true;
    }
    return false;
}

static size_t emit_zero_target_fault_skip_to_done(hb_codegen_buffer_t* buf, int target_reg) {
    size_t ok_branch;
    emit_cmp_imm(buf, target_reg, 0);
    ok_branch = emit_bcond_deferred(buf, 1); /* NE -> non-zero target */
    emit_mov_imm_compact(buf, 21, (uint32_t)HB_ERR_EXEC_FAULT);
    emit_str_w(buf, 21, 19, (uint32_t)offsetof(hb_context_t, last_result));
    size_t done_branch = emit_b_deferred(buf);
    patch_bcond(buf, ok_branch, 1, buf->size);
    return done_branch;
}

static bool emit_native_indirect_jmp(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (!instr || instr->op != HB_IR_JMP || instr->src1.type == HB_OP_NONE) return false;
    if (!emit_native_branch_target_to_x20(buf, &instr->src1)) return false;
    size_t done_branch = emit_zero_target_fault_skip_to_done(buf, 20);
    emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
    patch_b(buf, done_branch, buf->size);
    return true;
}

static bool emit_native_indirect_call(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (buf->arch == HB_ARCH_X86) return false;
    if (!instr || instr->op != HB_IR_CALL || instr->src1.type == HB_OP_NONE) return false;
    if (!jit_direct_stack_enabled()) return false;
    if (!emit_native_branch_target_to_x20(buf, &instr->src1)) return false;
    size_t done_branch = emit_zero_target_fault_skip_to_done(buf, 20);
    emit_mov_reg(buf, 23, 20);
    emit_mov_imm_compact(buf, 20, instr->guest_addr + instr->guest_len);
    emit_native_stack_push_x20(buf);
    emit_str_x(buf, 23, 19, (uint32_t)offsetof(hb_context_t, pc));
    patch_b(buf, done_branch, buf->size);
    return true;
}

static bool emit_native_bswap(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    hb_ir_operand_t dst;
    hb_size_t size;

    if (!instr || instr->op != HB_IR_BSWAP || !is_plain_gpr_reg_operand(&instr->dst))
        return false;

    dst = instr->dst;
    size = dst.size ? dst.size : HB_SIZE_32;
    if (size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;

    dst.size = size;
    if (!emit_load_gpr_sized_to_x20(buf, &dst))
        return false;
    if (size == HB_SIZE_64)
        emit_rev_x(buf, 20, 20);
    else
        emit_rev_w(buf, 20, 20);
    emit_store_x20_to_gpr_sized(buf, &dst);
    return true;
}

static void emit_clear_lazy_flags_pending(hb_codegen_buffer_t* buf) {
    emit_strb_w(buf, 31, 19, (uint32_t)(offsetof(hb_context_t, lazy_flags) +
                                        offsetof(hb_lazy_flags_t, pending)));
}

static void emit_store_flag_bool_from_w(hb_codegen_buffer_t* buf, int wreg, size_t flag_off) {
    emit_strb_w(buf, wreg, 19, (uint32_t)(offsetof(hb_context_t, flags) + flag_off));
}

static bool emit_load_bit_scan_source_to_x20(hb_codegen_buffer_t* buf,
                                             const hb_ir_instr_t* instr,
                                             hb_size_t size) {
    if (is_gpr_reg_operand(&instr->src1)) {
        if (!emit_load_gpr_sized_to_x20(buf, &instr->src1))
            return false;
    } else if (instr->src1.type == HB_OP_IMM) {
        emit_mov_imm_compact(buf, 20, (uint64_t)instr->src1.imm);
    } else if (jit_direct_mem_codegen_enabled(buf) &&
               direct_user_mem_allowed(buf, &instr->src1)) {
        uint32_t off = emit_direct_mem_addr_with_offset(buf, &instr->src1);
        emit_direct_mem_load_to_x20_off(buf, instr->src1.size ? instr->src1.size : size, off);
    } else {
        return false;
    }
    emit_mask_x_reg_to_size(buf, 20, 23, size);
    return true;
}

static bool emit_native_bit_scan(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    hb_ir_operand_t dst;
    hb_size_t size;
    uint64_t width;
    size_t zero_branch = 0;

    if (!instr || !is_plain_gpr_reg_operand(&instr->dst))
        return false;
    if (instr->op != HB_IR_BSF && instr->op != HB_IR_TZCNT &&
        instr->op != HB_IR_LZCNT && instr->op != HB_IR_BSR)
        return false;

    dst = instr->dst;
    size = dst.size ? dst.size : HB_SIZE_64;
    if (size != HB_SIZE_8 && size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;
    dst.size = size;
    width = (size == HB_SIZE_8) ? 8 : (size == HB_SIZE_16) ? 16 :
            (size == HB_SIZE_32) ? 32 : 64;

    if (!emit_load_bit_scan_source_to_x20(buf, instr, size))
        return false;

    emit_clear_lazy_flags_pending(buf);
    emit_cmp_imm(buf, 20, 0);
    emit_cset_w(buf, 22, 0); /* EQ: input was zero */

    if (instr->op == HB_IR_BSF || instr->op == HB_IR_BSR) {
        emit_store_flag_bool_from_w(buf, 22, offsetof(hb_flags_t, zf));
        zero_branch = emit_bcond_deferred(buf, 0); /* EQ: destination unchanged */
        if (instr->op == HB_IR_BSF) {
            emit_rbit_x(buf, 20, 20);
            emit_clz_x(buf, 20, 20);
        } else {
            emit_clz_x(buf, 20, 20);
            emit_mov_imm_compact(buf, 21, 63);
            emit_sub_reg(buf, 20, 21, 20);
        }
        emit_store_x20_to_gpr_sized(buf, &dst);
        patch_bcond(buf, zero_branch, 0, buf->size);
        return true;
    }

    emit_store_flag_bool_from_w(buf, 22, offsetof(hb_flags_t, cf));
    if (instr->op == HB_IR_TZCNT) {
        emit_mov_imm_compact(buf, 21, width);
        emit_rbit_x(buf, 20, 20);
        emit_clz_x(buf, 20, 20);
        emit_csel_x(buf, 20, 20, 21, 1); /* NE: count, else operand width */
    } else {
        emit_clz_x(buf, 20, 20);
        if (width != 64)
            emit_sub_imm(buf, 20, 20, (uint32_t)(64 - width));
    }
    emit_cmp_imm(buf, 20, 0);
    emit_cset_w(buf, 22, 0); /* ZF = result == 0 */
    emit_store_flag_bool_from_w(buf, 22, offsetof(hb_flags_t, zf));
    emit_store_x20_to_gpr_sized(buf, &dst);
    return true;
}

static bool emit_native_bit_test(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    hb_ir_operand_t target, bitop;
    hb_size_t size;
    uint64_t width;

    if (!instr || (instr->op != HB_IR_BT && instr->op != HB_IR_BTS &&
                   instr->op != HB_IR_BTR && instr->op != HB_IR_BTC))
        return false;
    if (!is_gpr_reg_operand(&instr->src1))
        return false;
    if (instr->src2.type != HB_OP_IMM && !is_gpr_reg_operand(&instr->src2))
        return false;

    target = instr->src1;
    size = target.size ? target.size : HB_SIZE_32;
    if (size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;
    target.size = size;
    width = (size == HB_SIZE_64) ? 64 : (size == HB_SIZE_16) ? 16 : 32;

    if (!emit_load_gpr_sized_to_reg(buf, &target, 20))
        return false;
    if (instr->src2.type == HB_OP_IMM) {
        emit_mov_imm_compact(buf, 21, (uint64_t)instr->src2.imm);
    } else {
        bitop = instr->src2;
        if (!bitop.size) bitop.size = HB_SIZE_64;
        if (!emit_load_gpr_sized_to_reg(buf, &bitop, 21))
            return false;
    }

    emit_mov_imm_compact(buf, 23, width - 1);
    emit_and_reg(buf, 21, 21, 23);
    emit_lsrv(buf, 22, 20, 21);
    emit_mov_imm_compact(buf, 23, 1);
    emit_and_reg(buf, 22, 22, 23);

    emit_clear_lazy_flags_pending(buf);
    emit_store_flag_bool_from_w(buf, 22, offsetof(hb_flags_t, cf));

    if (instr->op == HB_IR_BT)
        return true;

    emit_lslv(buf, 22, 23, 21);
    switch (instr->op) {
        case HB_IR_BTS:
            emit_orr_reg(buf, 20, 20, 22);
            break;
        case HB_IR_BTR:
            emit_mvn(buf, 22, 22);
            emit_and_reg(buf, 20, 20, 22);
            break;
        case HB_IR_BTC:
            emit_eor_reg(buf, 20, 20, 22);
            break;
        default:
            return false;
    }
    emit_store_x20_to_gpr_sized(buf, &target);
    return true;
}

static bool emit_native_cwd(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    hb_size_t size;
    uint64_t sign_bit;
    hb_ir_operand_t dst;

    if (!instr || instr->op != HB_IR_CWD)
        return false;

    size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
    if (size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;

    sign_bit = (size == HB_SIZE_16) ? 0x8000ULL :
               (size == HB_SIZE_32) ? 0x80000000ULL :
                                      0x8000000000000000ULL;
    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RAX));
    emit_mov_imm_compact(buf, 21, sign_bit);
    emit_and_reg(buf, 20, 20, 21);
    emit_cmp_imm(buf, 20, 0);
    emit_cset_w(buf, 20, 1); /* NE: sign bit was set */
    emit_neg(buf, 20, 20);   /* 0 -> 0, 1 -> all ones */
    if (size == HB_SIZE_32)
        emit_mask_x_reg_to_size(buf, 20, 21, HB_SIZE_32);

    dst = hb_ir_reg(HB_REG_RDX, size);
    emit_store_x20_to_gpr_sized(buf, &dst);
    return true;
}

static bool emit_hot_scalar_scan_loop(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t* add;
    const hb_ir_instr_t* cmp;
    const hb_ir_instr_t* jcc;
    hb_reg_t scan_reg;
    int branch_cond;
    size_t loop_off;
    int32_t loop_delta;

    if (!jit_direct_mem_codegen_enabled(buf) || !jit_direct_scalar_scan_enabled() ||
        !block || block->instr_count != 3)
        return false;

    add = &block->instrs[0];
    cmp = &block->instrs[1];
    jcc = &block->instrs[2];
    if (add->op != HB_IR_ADD || cmp->op != HB_IR_CMP || jcc->op != HB_IR_Jcc) return false;
    if (jcc->target != add->guest_addr) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!is_plain_gpr_reg_operand(&add->dst) || !is_plain_gpr_reg_operand(&add->src1)) return false;
    if (add->dst.reg != add->src1.reg || add->dst.size != HB_SIZE_64) return false;
    if (add->src2.type != HB_OP_IMM || add->src2.imm != 1) return false;
    if (cmp->src1.type != HB_OP_MEM || (cmp->src1.size != HB_SIZE_8 && cmp->src1.size != HB_SIZE_16))
        return false;
    if (!mem_operand_uses_reg(&cmp->src1, add->dst.reg)) return false;
    if (cmp->src2.type == HB_OP_REG && !is_gpr_reg_operand(&cmp->src2)) return false;
    if (cmp->src2.type != HB_OP_REG && cmp->src2.type != HB_OP_IMM) return false;

    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=direct-scalar-scan block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }

    scan_reg = add->dst.reg;
    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, scan_reg)); /* X20 = scan register */

    if (cmp->src2.type == HB_OP_REG) {
        if (cmp->src2.reg == scan_reg) emit_mov_reg(buf, 22, 20);
        else emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, cmp->src2.reg));
    } else {
        emit_mov_imm64(buf, 22, (uint64_t)cmp->src2.imm);
    }
    emit_mask_x_reg_to_size(buf, 22, 23, cmp->src1.size);

    loop_off = buf->size;
    emit_add_imm(buf, 20, 20, 1);
    if (!emit_direct_mem_addr_with_override(buf, &cmp->src1, scan_reg, 20))
        return false;
    emit_ldar_to_reg(buf, 23, 21, cmp->src1.size);
    emit_sub_reg(buf, 21, 23, 22);
    emit_cmp_reg(buf, 23, 22);
    branch_cond = arm64_cond(jcc->cc);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, branch_cond, loop_delta);

    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, scan_reg));
    emit_note_lazy_cmp_from_x23_x22_x21(buf, cmp->src1.size);
    emit_mov_imm64(buf, 21, jcc->guest_addr + jcc->guest_len);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static bool emit_scalar_operand_to_x20(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                       hb_size_t size) {
    if (!op) return false;
    if (op->type == HB_OP_REG) {
        hb_ir_operand_t sized = *op;
        sized.size = size;
        return emit_load_gpr_sized_to_reg(buf, &sized, 20);
    }
    if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 20, (uint64_t)op->imm);
        emit_mask_x_reg_to_size(buf, 20, 23, size);
        return true;
    }
    if (op->type == HB_OP_MEM && jit_direct_mem_codegen_enabled(buf) &&
        direct_user_mem_allowed(buf, op)) {
        uint32_t off = emit_direct_mem_addr_with_offset(buf, op);
        emit_direct_mem_load_to_x20_off(buf, size, off);
        return true;
    }
    return false;
}

static bool emit_scalar_operand_to_x21(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                        hb_size_t size) {
    if (!op) return false;
    if (op->type == HB_OP_REG) {
        hb_ir_operand_t sized = *op;
        sized.size = size;
        return emit_load_gpr_sized_to_reg(buf, &sized, 21);
    }
    if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 21, (uint64_t)op->imm);
        emit_mask_x_reg_to_size(buf, 21, 23, size);
        return true;
    }
    return false;
}

static bool emit_native_extend(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    if (!instr || (instr->op != HB_IR_ZERO_EXTEND && instr->op != HB_IR_SIGN_EXTEND))
        return false;
    if (!is_gpr_reg_operand(&instr->dst)) return false;
    if (instr->src1.type == HB_OP_REG && !is_gpr_reg_operand(&instr->src1)) return false;
    if (instr->src1.type == HB_OP_MEM &&
        (!jit_direct_mem_codegen_enabled(buf) ||
         !direct_user_mem_allowed(buf, &instr->src1)))
        return false;
    if (instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_MEM &&
        instr->src1.type != HB_OP_IMM)
        return false;
    if (!emit_scalar_operand_to_x20(buf, &instr->src1, instr->src1.size)) return false;

    if (instr->op == HB_IR_SIGN_EXTEND) {
        switch (instr->src1.size) {
            case HB_SIZE_8:  emit_sbfm(buf, 20, 20, 7); break;
            case HB_SIZE_16: emit_sbfm(buf, 20, 20, 15); break;
            case HB_SIZE_32: emit_sbfm(buf, 20, 20, 31); break;
            case HB_SIZE_64: break;
            default: return false;
        }
        emit_mask_x_reg_to_size(buf, 20, 23, instr->dst.size);
    } else {
        emit_mask_x_reg_to_size(buf, 20, 23, instr->src1.size);
    }
    emit_store_x20_to_gpr_sized(buf, &instr->dst);
    return true;
}

static bool emit_flags_set_pc(hb_codegen_buffer_t* buf, hb_cc_t cc,
                              uint64_t target, uint64_t fallthrough) {
    int64_t delta = (int64_t)target - (int64_t)fallthrough;
    if (cc != HB_CC_E && cc != HB_CC_NE) return false;
    if (delta >= -4095 && delta <= 4095) {
        emit_mov_imm_compact(buf, 21, fallthrough);
        emit_bcond(buf, arm64_cond(cc) ^ 1, 8);
        if (delta >= 0) emit_add_imm(buf, 21, 21, (uint32_t)delta);
        else emit_sub_imm(buf, 21, 21, (uint32_t)(-delta));
        emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
        return true;
    }
    emit_bcond(buf, arm64_cond(cc), 28);
    emit_mov_imm64(buf, 21, fallthrough);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
    emit_b(buf, 24);
    emit_mov_imm64(buf, 20, target);
    emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static bool emit_reg_zero_set_pc(hb_codegen_buffer_t* buf, int cmp_reg, hb_cc_t cc,
                                 uint64_t target, uint64_t fallthrough) {
    emit_cmp_imm(buf, cmp_reg, 0);
    return emit_flags_set_pc(buf, cc, target, fallthrough);
}

static bool emit_cmp_zero_set_pc(hb_codegen_buffer_t* buf, hb_cc_t cc,
                                 uint64_t target, uint64_t fallthrough) {
    return emit_reg_zero_set_pc(buf, 22, cc, target, fallthrough);
}

static void emit_set_pc_imm64(hb_codegen_buffer_t* buf, uint64_t pc) {
    emit_mov_imm_compact(buf, 21, pc);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
}

static void emit_note_lazy_cmp_zero_from_x20(hb_codegen_buffer_t* buf, hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    emit_stp_x(buf, 20, 31, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, lhs));
    emit_stp_x(buf, 20, 31, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, result));
    emit_note_lazy_header(buf, 16, 17, HB_LAZY_FLAGS_CMP, width);
    emit_note_lazy_masks(buf, 16, HB_FLAG_BIT_ALL);
}

static bool emit_scalar_flags_jcc_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                       const hb_ir_instr_t* jcc) {
    hb_lazy_flags_kind_t kind;
    hb_size_t size;
    bool writes_dst = false;
    if (!op || !jcc || jcc->op != HB_IR_Jcc) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!lazy_kind_for_scalar_op(op->op, &kind)) return false;

    if (op->op == HB_IR_ADD || op->op == HB_IR_SUB ||
        op->op == HB_IR_AND || op->op == HB_IR_OR || op->op == HB_IR_XOR) {
        if (!is_plain_gpr_reg_operand(&op->dst) || !is_plain_gpr_reg_operand(&op->src1) ||
            (op->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&op->src2)) ||
            (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM))
            return false;
        size = op->dst.size;
        writes_dst = true;
    } else if (op->op == HB_IR_CMP || op->op == HB_IR_TEST) {
        if (op->src1.type == HB_OP_REG && op->src1.reg_offset != 0) return false;
        if (op->src2.type == HB_OP_REG && op->src2.reg_offset != 0) return false;
        if (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM) return false;
        size = op->src1.size;
    } else {
        return false;
    }

    if (!emit_scalar_operand_to_x20(buf, &op->src1, size)) return false;
    if (!emit_scalar_operand_to_x21(buf, &op->src2, size)) return false;
    switch (op->op) {
        case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
        case HB_IR_SUB:
        case HB_IR_CMP: emit_sub_reg(buf, 22, 20, 21); break;
        case HB_IR_AND:
        case HB_IR_TEST: emit_and_reg(buf, 22, 20, 21); break;
        case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
        case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
        default: return false;
    }
    emit_mask_x_reg_to_size(buf, 22, 23, size);
    emit_note_lazy_from_x20_x21_x22(buf, kind, size);
    if (writes_dst) {
        emit_mov_reg(buf, 20, 22);
        emit_store_x20_to_gpr_sized(buf, &op->dst);
    }
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_scalar_flags_result_to_x22(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op) {
    hb_lazy_flags_kind_t kind;
    hb_size_t size;
    bool writes_dst = false;
    if (!op || !lazy_kind_for_scalar_op(op->op, &kind)) return false;

    if (op->op == HB_IR_ADD || op->op == HB_IR_SUB ||
        op->op == HB_IR_AND || op->op == HB_IR_OR || op->op == HB_IR_XOR) {
        if (!is_plain_gpr_reg_operand(&op->dst) || !is_plain_gpr_reg_operand(&op->src1) ||
            (op->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&op->src2)) ||
            (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM))
            return false;
        size = op->dst.size;
        writes_dst = true;
    } else if (op->op == HB_IR_CMP || op->op == HB_IR_TEST) {
        if (op->src1.type == HB_OP_REG && op->src1.reg_offset != 0) return false;
        if (op->src2.type == HB_OP_REG && op->src2.reg_offset != 0) return false;
        if (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM) return false;
        size = op->src1.size;
    } else {
        return false;
    }
    if (size != HB_SIZE_8 && size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;

    if (!emit_scalar_operand_to_x20(buf, &op->src1, size)) return false;
    if (!emit_scalar_operand_to_x21(buf, &op->src2, size)) return false;
    switch (op->op) {
        case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
        case HB_IR_SUB:
        case HB_IR_CMP: emit_sub_reg(buf, 22, 20, 21); break;
        case HB_IR_AND:
        case HB_IR_TEST: emit_and_reg(buf, 22, 20, 21); break;
        case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
        case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
        default: return false;
    }
    emit_mask_x_reg_to_size(buf, 22, 23, size);
    emit_note_lazy_from_x20_x21_x22(buf, kind, size);
    if (writes_dst) {
        emit_mov_reg(buf, 20, 22);
        emit_store_x20_to_gpr_sized(buf, &op->dst);
    }
    return true;
}

static bool can_emit_flags_safe_scalar_mov(const hb_ir_instr_t* instr) {
    if (!instr || instr->op != HB_IR_MOV) return false;
    if (!is_gpr_reg_operand(&instr->dst)) return false;
    if (instr->src1.type == HB_OP_REG) return is_gpr_reg_operand(&instr->src1);
    return instr->src1.type == HB_OP_IMM;
}

static bool emit_store_setcc_w20(hb_codegen_buffer_t* buf, const hb_ir_operand_t* dst) {
    if (!dst || dst->size != HB_SIZE_8) return false;
    if (dst->type == HB_OP_REG && is_gpr_reg_operand(dst)) {
        emit_store_x20_to_gpr_sized(buf, dst);
        return true;
    }
    if (dst->type == HB_OP_MEM && jit_direct_mem_codegen_enabled(buf) &&
        direct_user_mem_allowed(buf, dst)) {
        uint32_t off = emit_direct_mem_addr_with_offset(buf, dst);
        emit_direct_mem_store_from_x20_off(buf, HB_SIZE_8, off);
        return true;
    }
    return false;
}

static bool emit_scalar_flags_setcc_sequence(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                             const hb_ir_instr_t* maybe_mov,
                                             const hb_ir_instr_t* setcc) {
    if (!setcc || setcc->op != HB_IR_SETcc) return false;
    if (setcc->cc != HB_CC_E && setcc->cc != HB_CC_NE) return false;
    if (!((setcc->dst.type == HB_OP_REG && is_gpr_reg_operand(&setcc->dst)) ||
          (setcc->dst.type == HB_OP_MEM && jit_direct_mem_codegen_enabled(buf) &&
           direct_user_mem_allowed(buf, &setcc->dst))))
        return false;
    if (setcc->dst.size != HB_SIZE_8) return false;
    if (maybe_mov && !can_emit_flags_safe_scalar_mov(maybe_mov)) return false;

    if (!emit_scalar_flags_result_to_x22(buf, op)) return false;
    if (maybe_mov && !emit_native_scalar_mov(buf, maybe_mov)) return false;
    emit_cmp_imm(buf, 22, 0);
    emit_cset_w(buf, 20, arm64_cond(setcc->cc));
    return emit_store_setcc_w20(buf, &setcc->dst);
}

static bool emit_test_same_reg_jcc_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                        const hb_ir_instr_t* jcc) {
    hb_size_t size;
    if (!op || !jcc || op->op != HB_IR_TEST || jcc->op != HB_IR_Jcc) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!same_plain_gpr_operand(&op->src1, &op->src2)) return false;
    size = op->src1.size;
    if (size != HB_SIZE_8 && size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;

    if (!emit_load_gpr_sized_to_reg(buf, &op->src1, 20)) return false;
    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_mem_imm_flags_jcc_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                         const hb_ir_instr_t* jcc) {
    hb_lazy_flags_kind_t kind;
    hb_size_t size;
    if (!op || !jcc || jcc->op != HB_IR_Jcc) return false;
    if (op->op != HB_IR_TEST && op->op != HB_IR_CMP) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (op->src1.type != HB_OP_MEM || !direct_user_mem_allowed(buf, &op->src1))
        return false;
    if (op->src2.type != HB_OP_IMM) return false;
    size = op->src1.size;
    if (size != HB_SIZE_8 && size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;
    if (!lazy_kind_for_scalar_op(op->op, &kind)) return false;

    uint32_t off = emit_direct_mem_addr_with_offset(buf, &op->src1);
    emit_direct_mem_load_to_x20_off(buf, size, off);
    if (op->op == HB_IR_CMP && op->src2.imm == 0) {
        emit_note_lazy_cmp_zero_from_x20(buf, size);
        return emit_reg_zero_set_pc(buf, 20, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
    }
    emit_mov_imm_compact(buf, 21, (uint64_t)op->src2.imm);
    if (!imm_fits_size((uint64_t)op->src2.imm, size))
        emit_mask_x_reg_to_size(buf, 21, 23, size);
    if (op->op == HB_IR_TEST) {
        emit_ands_reg(buf, 22, 20, 21);
        emit_note_lazy_from_x20_x21_x22(buf, kind, size);
        return emit_flags_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
    } else {
        emit_sub_reg(buf, 22, 20, 21);
        emit_mask_x_reg_to_size(buf, 22, 23, size);
    }
    emit_note_lazy_from_x20_x21_x22(buf, kind, size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_mem_reg_flags_jcc_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                         const hb_ir_instr_t* jcc) {
    hb_lazy_flags_kind_t kind;
    hb_size_t size;
    if (!op || !jcc || jcc->op != HB_IR_Jcc) return false;
    if (op->op != HB_IR_TEST && op->op != HB_IR_CMP) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (op->src1.type != HB_OP_MEM || !direct_user_mem_allowed(buf, &op->src1))
        return false;
    if (!is_plain_gpr_reg_operand(&op->src2)) return false;
    size = op->src1.size;
    if (op->src2.size != size) return false;
    if (size != HB_SIZE_8 && size != HB_SIZE_16 && size != HB_SIZE_32 && size != HB_SIZE_64)
        return false;
    if (!lazy_kind_for_scalar_op(op->op, &kind)) return false;

    uint32_t off = emit_direct_mem_addr_with_offset(buf, &op->src1);
    emit_direct_mem_load_to_x20_off(buf, size, off);
    if (!emit_load_gpr_sized_to_reg(buf, &op->src2, 21)) return false;
    if (op->op == HB_IR_TEST) {
        emit_ands_reg(buf, 22, 20, 21);
        emit_note_lazy_from_x20_x21_x22(buf, kind, size);
        return emit_flags_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
    }

    emit_sub_reg(buf, 22, 20, 21);
    emit_mask_x_reg_to_size(buf, 22, 23, size);
    emit_note_lazy_from_x20_x21_x22(buf, kind, size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_adjacent_mem64_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* first,
                                     const hb_ir_instr_t* second) {
    if (!jit_direct_mem_codegen_enabled(buf) || !first || !second) return false;

    if (first->op == HB_IR_STORE && second->op == HB_IR_STORE &&
        adjacent_mem64_operands(buf, &first->src1, &second->src1) &&
        is_plain_gpr_reg_operand(&first->src2) && first->src2.size == HB_SIZE_64 &&
        is_plain_gpr_reg_operand(&second->src2) && second->src2.size == HB_SIZE_64) {
        uint32_t off = 0;
        if (adjacent_mem64_pair_offset(buf, &first->src1, &off)) {
            emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, first->src1.mem.base));
        } else {
            emit_direct_mem_addr(buf, &first->src1);
        }
        emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, first->src2.reg));
        emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, second->src2.reg));
        emit_stp_x(buf, 20, 22, 21, off);
        return true;
    }

    if (first->op == HB_IR_LOAD && second->op == HB_IR_LOAD &&
        adjacent_mem64_operands(buf, &first->src1, &second->src1) &&
        is_plain_gpr_reg_operand(&first->dst) && first->dst.size == HB_SIZE_64 &&
        is_plain_gpr_reg_operand(&second->dst) && second->dst.size == HB_SIZE_64) {
        uint32_t off = 0;
        if (adjacent_mem64_pair_offset(buf, &first->src1, &off)) {
            emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, first->src1.mem.base));
        } else {
            emit_direct_mem_addr(buf, &first->src1);
        }
        emit_ldp_x(buf, 20, 22, 21, off);
        emit_store_x20_to_gpr_sized(buf, &first->dst);
        emit_mov_reg(buf, 20, 22);
        emit_store_x20_to_gpr_sized(buf, &second->dst);
        return true;
    }

    return false;
}

static bool emit_xmm_load_store_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* load,
                                     const hb_ir_instr_t* store) {
    if (!jit_direct_mem_codegen_enabled(buf) || !load || !store) return false;
    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE) return false;
    if (!is_xmm_reg_operand(&load->dst) || !is_xmm_reg_operand(&store->src2) ||
        load->dst.reg != store->src2.reg)
        return false;
    if (!direct_user_xmm_mem_allowed(buf, &load->src1) ||
        !direct_user_xmm_mem_allowed(buf, &store->src1))
        return false;

    emit_direct_mem_addr(buf, &load->src1);
    emit_direct_mem128_load_to_x20_x22(buf);
    emit_store_x20_x22_to_xmm(buf, load->dst.reg);
    if (direct_mem_addr_preserves_x22(&store->src1)) {
        emit_direct_mem_addr(buf, &store->src1);
    } else {
        emit_mov_reg(buf, 23, 22);
        emit_direct_mem_addr(buf, &store->src1);
        emit_mov_reg(buf, 22, 23);
    }
    emit_direct_mem128_store_from_x20_x22(buf);
    return true;
}

static bool emit_scalar_load_store_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* load,
                                        const hb_ir_instr_t* store) {
    if (!jit_direct_mem_codegen_enabled(buf) || !load || !store) return false;
    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE) return false;
    if (!is_plain_gpr_reg_operand(&load->dst) || !is_plain_gpr_reg_operand(&store->src2) ||
        load->dst.reg != store->src2.reg || load->dst.size != store->src2.size)
        return false;
    if (!direct_user_mem_allowed(buf, &load->src1) ||
        !direct_user_mem_allowed(buf, &store->src1))
        return false;
    if (load->src1.size != load->dst.size || store->src1.size != store->src2.size)
        return false;

    uint32_t load_off = emit_direct_mem_addr_with_offset(buf, &load->src1);
    emit_direct_mem_load_to_x20_off(buf, load->src1.size, load_off);
    emit_store_x20_to_gpr_sized(buf, &load->dst);
    /* Alignment-checked TSO store (raw STLR SIGBUSes on an unaligned dest, which x86 allows). */
    if (!emit_direct_mem_store_from_x20_tso(buf, &store->src1)) return false;
    return true;
}

static bool emit_stack_spill_push_sub_prologue(hb_codegen_buffer_t* buf,
                                               const hb_ir_instr_t* store1,
                                               const hb_ir_instr_t* store2,
                                               const hb_ir_instr_t* push,
                                               const hb_ir_instr_t* sub) {
    uint64_t frame;
    uint32_t push_off, pair_off;
    if (!jit_direct_mem_codegen_enabled(buf) || !store1 || !store2 || !push || !sub)
        return false;
    if (!stack_store_reg64_at(store1, 8) || !stack_store_reg64_at(store2, 16))
        return false;
    if (push->op != HB_IR_PUSH || push->src1.type != HB_OP_REG ||
        !is_plain_gpr_reg_operand(&push->src1) || push->src1.size != HB_SIZE_64)
        return false;
    if (sub->op != HB_IR_SUB ||
        !is_plain_gpr_reg_operand(&sub->dst) || !is_plain_gpr_reg_operand(&sub->src1) ||
        sub->dst.reg != HB_REG_RSP || sub->src1.reg != HB_REG_RSP ||
        sub->dst.size != HB_SIZE_64 || sub->src1.size != HB_SIZE_64 ||
        sub->src2.type != HB_OP_IMM || sub->src2.imm < 0)
        return false;
    frame = (uint64_t)sub->src2.imm;
    if (frame > 480 || (frame & 7)) return false;
    push_off = (uint32_t)frame;
    pair_off = (uint32_t)frame + 16;

    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, store1->src2.reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, store2->src2.reg));
    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, push->src1.reg));
    emit_sub_imm(buf, 20, 20, (uint32_t)(frame + 8));
    emit_str_x(buf, 23, 20, push_off);
    emit_stp_x(buf, 21, 22, 20, pair_off);
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));

    emit_mov_reg(buf, 22, 20);
    emit_add_imm(buf, 20, 22, (uint32_t)frame);
    emit_mov_imm_compact(buf, 21, frame);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_SUB, HB_SIZE_64);
    return true;
}

static bool emit_prologue_local_init_test_block(hb_codegen_buffer_t* buf,
                                                const hb_ir_block_t* block) {
    const hb_ir_instr_t *store1, *store2, *push, *sub, *store_zero, *mov, *lea, *test, *jcc;
    uint64_t frame;
    uint32_t store_off, push_off, pair_off;
    if (!jit_direct_mem_codegen_enabled(buf) || !block || block->instr_count != 9)
        return false;

    store1 = &block->instrs[0];
    store2 = &block->instrs[1];
    push = &block->instrs[2];
    sub = &block->instrs[3];
    store_zero = &block->instrs[4];
    mov = &block->instrs[5];
    lea = &block->instrs[6];
    test = &block->instrs[7];
    jcc = &block->instrs[8];

    if (!stack_store_reg64_at(store1, 8) || !stack_store_reg64_at(store2, 16))
        return false;
    if (push->op != HB_IR_PUSH || push->src1.type != HB_OP_REG ||
        !is_plain_gpr_reg_operand(&push->src1) || push->src1.size != HB_SIZE_64)
        return false;
    if (sub->op != HB_IR_SUB ||
        !is_plain_gpr_reg_operand(&sub->dst) || !is_plain_gpr_reg_operand(&sub->src1) ||
        sub->dst.reg != HB_REG_RSP || sub->src1.reg != HB_REG_RSP ||
        sub->dst.size != HB_SIZE_64 || sub->src1.size != HB_SIZE_64 ||
        sub->src2.type != HB_OP_IMM || sub->src2.imm < 0)
        return false;
    frame = (uint64_t)sub->src2.imm;
    if (frame > 480 || (frame & 7)) return false;
    if (store_zero->op != HB_IR_STORE || store_zero->src1.type != HB_OP_MEM ||
        store_zero->src1.size != HB_SIZE_8 || store_zero->src2.type != HB_OP_IMM ||
        store_zero->src2.imm != 0)
        return false;
    if (!direct_mem_unsigned_offset(buf, &store_zero->src1, &store_off))
        return false;
    if (!is_plain_gpr_reg_operand(&mov->dst) || !is_plain_gpr_reg_operand(&mov->src1) ||
        mov->op != HB_IR_MOV || mov->dst.size != HB_SIZE_64 || mov->src1.size != HB_SIZE_64)
        return false;
    if (store_zero->src1.mem.base != mov->src1.reg)
        return false;
    if (!is_plain_gpr_reg_operand(&lea->dst) || lea->op != HB_IR_LEA ||
        lea->dst.size != HB_SIZE_64 || lea->src1.type != HB_OP_MEM ||
        lea->src1.mem.base != mov->src1.reg || lea->src1.mem.index != HB_REG_COUNT ||
        lea->src1.mem.segment != 0 || lea->src1.mem.addr32 ||
        lea->src1.mem.disp < 0 || lea->src1.mem.disp >= 4096)
        return false;
    if (test->op != HB_IR_TEST || !same_plain_gpr_operand(&test->src1, &test->src2) ||
        test->src1.size != HB_SIZE_64 || jcc->op != HB_IR_Jcc ||
        (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE))
        return false;

    push_off = (uint32_t)frame;
    pair_off = (uint32_t)frame + 16;
    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, store1->src2.reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, store2->src2.reg));
    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, push->src1.reg));
    emit_sub_imm(buf, 20, 20, (uint32_t)(frame + 8));
    emit_str_x(buf, 23, 20, push_off);
    emit_stp_x(buf, 21, 22, 20, pair_off);
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));

    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, mov->src1.reg));
    emit_mov_imm_compact(buf, 20, 0);
    emit_direct_mem_store_from_x20_off(buf, store_zero->src1.size, store_off);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, mov->dst.reg));
    if (lea->src1.mem.disp) emit_add_imm(buf, 21, 21, (uint32_t)lea->src1.mem.disp);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, lea->dst.reg));

    if (!emit_load_gpr_sized_to_reg(buf, &test->src1, 20)) return false;
    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, test->src1.size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_epilogue_restore_ret_block(hb_codegen_buffer_t* buf,
                                            const hb_ir_block_t* block) {
    const hb_ir_instr_t *load1, *maybe_mov, *load2, *add_rsp, *pop, *ret;
    hb_reg_t load1_reg, load2_reg;
    uint32_t load1_disp, load2_disp;
    uint64_t frame, ret_adjust = 0, final_adjust;
    size_t idx = 0;
    if (!jit_direct_mem_codegen_enabled(buf) || !block ||
        (block->instr_count != 5 && block->instr_count != 6))
        return false;

    load1 = &block->instrs[idx++];
    if (!stack_load_reg64(load1, &load1_reg, &load1_disp)) return false;

    maybe_mov = NULL;
    load2 = &block->instrs[idx++];
    if (block->instr_count == 6) {
        maybe_mov = load2;
        load2 = &block->instrs[idx++];
        if (maybe_mov->op != HB_IR_MOV ||
            !is_plain_gpr_reg_operand(&maybe_mov->dst) ||
            !is_plain_gpr_reg_operand(&maybe_mov->src1) ||
            maybe_mov->dst.size != HB_SIZE_64 || maybe_mov->src1.size != HB_SIZE_64)
            return false;
    }
    if (!stack_load_reg64(load2, &load2_reg, &load2_disp)) return false;

    add_rsp = &block->instrs[idx++];
    pop = &block->instrs[idx++];
    ret = &block->instrs[idx++];
    if (add_rsp->op != HB_IR_ADD ||
        !is_plain_gpr_reg_operand(&add_rsp->dst) ||
        !is_plain_gpr_reg_operand(&add_rsp->src1) ||
        add_rsp->dst.reg != HB_REG_RSP || add_rsp->src1.reg != HB_REG_RSP ||
        add_rsp->dst.size != HB_SIZE_64 || add_rsp->src1.size != HB_SIZE_64 ||
        add_rsp->src2.type != HB_OP_IMM || add_rsp->src2.imm < 0)
        return false;
    frame = (uint64_t)add_rsp->src2.imm;
    if (frame > 4095 || (frame & 7)) return false;
    if (pop->op != HB_IR_POP || !is_plain_gpr_reg_operand(&pop->dst) ||
        pop->dst.size != HB_SIZE_64)
        return false;
    if (ret->op != HB_IR_RET) return false;
    if (ret->src1.type == HB_OP_IMM) {
        if (ret->src1.imm < 0) return false;
        ret_adjust = (uint64_t)ret->src1.imm;
    } else if (!operand_is_none_or_unset(&ret->src1)) {
        return false;
    }
    final_adjust = frame + 16 + ret_adjust;
    if (final_adjust > 4095) return false;

    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_ldr_x(buf, 21, 20, load1_disp);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, load1_reg));
    if (maybe_mov) {
        emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, maybe_mov->src1.reg));
        emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, maybe_mov->dst.reg));
    }
    emit_ldr_x(buf, 21, 20, load2_disp);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, load2_reg));
    emit_ldr_x(buf, 21, 20, (uint32_t)frame);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, pop->dst.reg));
    emit_ldr_x(buf, 22, 20, (uint32_t)(frame + 8));
    emit_add_imm(buf, 20, 20, (uint32_t)final_adjust);
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_RSP));
    emit_str_x(buf, 22, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static bool emit_mov_lea_same_base_pair(hb_codegen_buffer_t* buf,
                                        const hb_ir_instr_t* mov,
                                        const hb_ir_instr_t* lea) {
    if (!mov || !lea || mov->op != HB_IR_MOV || lea->op != HB_IR_LEA) return false;
    if (!is_plain_gpr_reg_operand(&mov->dst) || !is_plain_gpr_reg_operand(&mov->src1) ||
        mov->dst.size != HB_SIZE_64 || mov->src1.size != HB_SIZE_64)
        return false;
    if (!is_plain_gpr_reg_operand(&lea->dst) || lea->dst.size != HB_SIZE_64)
        return false;
    if (lea->src1.type != HB_OP_MEM || lea->src1.mem.base != mov->src1.reg ||
        lea->src1.mem.index != HB_REG_COUNT || lea->src1.mem.segment != 0 ||
        lea->src1.mem.addr32 || lea->src1.mem.disp < 0 || lea->src1.mem.disp >= 4096)
        return false;

    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, mov->src1.reg));
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, mov->dst.reg));
    if (lea->src1.mem.disp) emit_add_imm(buf, 20, 20, (uint32_t)lea->src1.mem.disp);
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, lea->dst.reg));
    return true;
}

static bool emit_store_imm_mov_lea_same_base(hb_codegen_buffer_t* buf,
                                             const hb_ir_instr_t* store,
                                             const hb_ir_instr_t* mov,
                                             const hb_ir_instr_t* lea) {
    uint32_t store_off;
    if (!store || !mov || !lea || store->op != HB_IR_STORE || mov->op != HB_IR_MOV ||
        lea->op != HB_IR_LEA)
        return false;
    if (store->src1.type != HB_OP_MEM || store->src2.type != HB_OP_IMM)
        return false;
    if (!direct_mem_unsigned_offset(buf, &store->src1, &store_off))
        return false;
    if (!is_plain_gpr_reg_operand(&mov->dst) || !is_plain_gpr_reg_operand(&mov->src1) ||
        mov->dst.size != HB_SIZE_64 || mov->src1.size != HB_SIZE_64 ||
        store->src1.mem.base != mov->src1.reg)
        return false;
    if (!is_plain_gpr_reg_operand(&lea->dst) || lea->dst.size != HB_SIZE_64)
        return false;
    if (lea->src1.type != HB_OP_MEM || lea->src1.mem.base != mov->src1.reg ||
        lea->src1.mem.index != HB_REG_COUNT || lea->src1.mem.segment != 0 ||
        lea->src1.mem.addr32 || lea->src1.mem.disp < 0 || lea->src1.mem.disp >= 4096)
        return false;

    emit_ldr_x(buf, 21, 19, (uint32_t)reg_off(buf, mov->src1.reg));
    emit_mov_imm_compact(buf, 20, (uint64_t)store->src2.imm);
    emit_direct_mem_store_from_x20_off(buf, store->src1.size, store_off);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, mov->dst.reg));
    if (lea->src1.mem.disp) emit_add_imm(buf, 21, 21, (uint32_t)lea->src1.mem.disp);
    emit_str_x(buf, 21, 19, (uint32_t)reg_off(buf, lea->dst.reg));
    return true;
}

static bool emit_zero_test_jcc_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t *xor_i, *test_i, *jcc;
    uint64_t next_pc;

    if (!block || block->instr_count != 3) return false;
    xor_i = &block->instrs[0];
    test_i = &block->instrs[1];
    jcc = &block->instrs[2];
    if (xor_i->op != HB_IR_XOR || test_i->op != HB_IR_TEST || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!same_plain_gpr_operand(&xor_i->dst, &xor_i->src1) ||
        !same_plain_gpr_operand(&xor_i->dst, &xor_i->src2))
        return false;
    if (!same_plain_gpr_operand(&xor_i->dst, &test_i->src1) ||
        !same_plain_gpr_operand(&xor_i->dst, &test_i->src2))
        return false;

    emit_mov_imm_compact(buf, 20, 0);
    emit_store_x20_to_gpr_sized(buf, &xor_i->dst);
    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, test_i->src1.size);

    next_pc = (jcc->cc == HB_CC_E) ? jcc->target : (jcc->guest_addr + jcc->guest_len);
    emit_set_pc_imm64(buf, next_pc);
    return true;
}

static bool emit_copy_scan_body_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t *load, *store, *add, *test, *jcc;
    hb_ir_operand_t inc_reg_operand;
    if (!jit_direct_mem_codegen_enabled(buf) || !block || block->instr_count != 5)
        return false;

    load = &block->instrs[0];
    store = &block->instrs[1];
    add = &block->instrs[2];
    test = &block->instrs[3];
    jcc = &block->instrs[4];
    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE ||
        add->op != HB_IR_ADD || test->op != HB_IR_TEST || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!is_plain_gpr_reg_operand(&load->dst)) return false;
    if (load->dst.size != HB_SIZE_8 && load->dst.size != HB_SIZE_16) return false;
    if (!direct_user_mem_allowed(buf, &load->src1) ||
        !direct_user_mem_allowed(buf, &store->src1))
        return false;
    if (!same_plain_gpr_operand(&load->dst, &store->src2)) return false;
    if (store->src1.size != load->dst.size) return false;
    if (!same_plain_gpr_operand(&load->dst, &test->src1) ||
        !same_plain_gpr_operand(&load->dst, &test->src2))
        return false;
    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (add->dst.reg == load->dst.reg) return false;

    uint32_t load_off = emit_direct_mem_addr_with_offset(buf, &load->src1);
    emit_direct_mem_load_to_x20_off(buf, load->dst.size, load_off);
    emit_store_x20_to_gpr_sized(buf, &load->dst);
    uint32_t store_off = emit_direct_mem_addr_with_offset(buf, &store->src1);
    emit_direct_mem_store_from_x20_off(buf, store->src1.size, store_off);

    inc_reg_operand = add->dst;
    if (!emit_load_gpr_sized_to_reg(buf, &inc_reg_operand, 22)) return false;
    emit_add_imm(buf, 22, 22, 1);
    emit_str_x(buf, 22, 19, (uint32_t)reg_off(buf, add->dst.reg));

    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, load->dst.size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static const hb_ir_block_t* find_cfg_block_for_codegen(const hb_ir_cfg_t* cfg, uint64_t addr) {
    if (!cfg) return NULL;
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i] && cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

static bool direct_mem_addr_preserves_override(hb_codegen_buffer_t* buf,
                                               const hb_ir_operand_t* op,
                                               hb_reg_t reg) {
    if (!direct_user_mem_allowed(buf, op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;
    if (!mem_operand_uses_reg(op, reg)) return false;
    if (op->mem.disp <= -4096 || op->mem.disp >= 4096) return false;
    if (op->mem.base == reg) {
        return op->mem.index == HB_REG_COUNT || op->mem.index == reg;
    }
    return op->mem.index == reg;
}

static bool emit_copy_scan_counted_loop_block(hb_codegen_buffer_t* buf,
                                              const hb_ir_block_t* body,
                                              const hb_ir_block_t* guard) {
    const hb_ir_instr_t *load, *store, *add, *test, *body_jcc;
    const hb_ir_instr_t *guard_sub, *guard_jcc;
    hb_reg_t inc_reg, count_reg;
    size_t loop_off, test_exit_branch, count_done_branch, test_exit_off, done_off;
    int32_t loop_delta;

    if (!jit_direct_mem_codegen_enabled(buf) || !body || !guard) return false;
    if (body->instr_count != 5 || guard->instr_count != 2) return false;

    load = &body->instrs[0];
    store = &body->instrs[1];
    add = &body->instrs[2];
    test = &body->instrs[3];
    body_jcc = &body->instrs[4];
    guard_sub = &guard->instrs[0];
    guard_jcc = &guard->instrs[1];

    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE ||
        add->op != HB_IR_ADD || test->op != HB_IR_TEST || body_jcc->op != HB_IR_Jcc ||
        guard_sub->op != HB_IR_SUB || guard_jcc->op != HB_IR_Jcc)
        return false;
    if (body_jcc->cc != HB_CC_E && body_jcc->cc != HB_CC_NE) return false;
    if (guard_jcc->cc != HB_CC_E && guard_jcc->cc != HB_CC_NE) return false;
    if (body_jcc->guest_addr + body_jcc->guest_len != guard->guest_addr) return false;
    if (guard_jcc->target != body->guest_addr) return false;

    if (!is_plain_gpr_reg_operand(&load->dst)) return false;
    if (load->dst.size != HB_SIZE_8 && load->dst.size != HB_SIZE_16) return false;
    if (!direct_user_mem_allowed(buf, &load->src1) ||
        !direct_user_mem_allowed(buf, &store->src1))
        return false;
    if (store->src1.size != load->dst.size) return false;
    if (!same_plain_gpr_operand(&load->dst, &store->src2)) return false;
    if (!same_plain_gpr_operand(&load->dst, &test->src1) ||
        !same_plain_gpr_operand(&load->dst, &test->src2))
        return false;

    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (!is_plain_gpr_reg_operand(&guard_sub->dst) ||
        !same_plain_gpr_operand(&guard_sub->dst, &guard_sub->src1) ||
        guard_sub->dst.size != HB_SIZE_64 ||
        guard_sub->src2.type != HB_OP_IMM || guard_sub->src2.imm != 1)
        return false;

    inc_reg = add->dst.reg;
    count_reg = guard_sub->dst.reg;
    if (inc_reg == load->dst.reg || count_reg == load->dst.reg || count_reg == inc_reg)
        return false;
    if (mem_operand_uses_reg(&load->src1, load->dst.reg) ||
        mem_operand_uses_reg(&store->src1, load->dst.reg) ||
        mem_operand_uses_reg(&load->src1, count_reg) ||
        mem_operand_uses_reg(&store->src1, count_reg))
        return false;
    if (!direct_mem_addr_preserves_override(buf, &load->src1, inc_reg) ||
        !direct_mem_addr_preserves_override(buf, &store->src1, inc_reg))
        return false;

    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, inc_reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, count_reg));

    loop_off = buf->size;
    if (!emit_direct_mem_addr_with_override(buf, &load->src1, inc_reg, 23)) return false;
    emit_direct_mem_load_to_x20(buf, load->dst.size);
    if (!emit_direct_mem_addr_with_override(buf, &store->src1, inc_reg, 23)) return false;
    emit_direct_mem_store_from_x20(buf, store->src1.size);
    emit_add_imm(buf, 23, 23, 1);

    emit_cmp_imm(buf, 20, 0);
    test_exit_branch = emit_bcond_deferred(buf, arm64_cond(body_jcc->cc));

    emit_mov_reg(buf, 21, 22);
    emit_subs_imm(buf, 22, 22, 1);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(guard_jcc->cc), loop_delta);

    emit_store_x20_to_gpr_sized(buf, &load->dst);
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, inc_reg));
    emit_str_x(buf, 22, 19, (uint32_t)reg_off(buf, count_reg));
    emit_mov_reg(buf, 20, 21);
    emit_mov_imm_compact(buf, 21, 1);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_SUB, guard_sub->dst.size);
    emit_set_pc_imm64(buf, guard_jcc->guest_addr + guard_jcc->guest_len);
    count_done_branch = emit_b_deferred(buf);

    test_exit_off = buf->size;
    patch_bcond(buf, test_exit_branch, arm64_cond(body_jcc->cc), test_exit_off);
    emit_store_x20_to_gpr_sized(buf, &load->dst);
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, inc_reg));
    emit_str_x(buf, 22, 19, (uint32_t)reg_off(buf, count_reg));
    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, load->dst.size);
    emit_set_pc_imm64(buf, body_jcc->target);

    done_off = buf->size;
    patch_b(buf, count_done_branch, done_off);
    return true;
}

static bool emit_bounded_scan_loop_block(hb_codegen_buffer_t* buf,
                                         const hb_ir_block_t* guard,
                                         const hb_ir_block_t* body) {
    const hb_ir_instr_t *guard_cmp, *guard_jcc;
    const hb_ir_instr_t *add, *body_cmp, *body_jcc;
    hb_reg_t scan_reg, limit_reg;
    size_t loop_off, guard_exit_branch, body_done_branch, guard_exit_off, done_off;
    int32_t loop_delta;

    if (!jit_direct_mem_codegen_enabled(buf) || !guard || !body) return false;
    if (guard->instr_count != 2 || body->instr_count != 3) return false;

    guard_cmp = &guard->instrs[0];
    guard_jcc = &guard->instrs[1];
    add = &body->instrs[0];
    body_cmp = &body->instrs[1];
    body_jcc = &body->instrs[2];

    if (guard_cmp->op != HB_IR_CMP || guard_jcc->op != HB_IR_Jcc ||
        add->op != HB_IR_ADD || body_cmp->op != HB_IR_CMP || body_jcc->op != HB_IR_Jcc)
        return false;
    if (guard_jcc->cc != HB_CC_E || body_jcc->cc != HB_CC_NE) return false;
    if (guard_jcc->guest_addr + guard_jcc->guest_len != body->guest_addr) return false;
    if (body_jcc->target != guard->guest_addr) return false;
    if (body_jcc->guest_addr + body_jcc->guest_len != guard_jcc->target) return false;

    if (!is_plain_gpr_reg_operand(&guard_cmp->src1) ||
        !is_plain_gpr_reg_operand(&guard_cmp->src2) ||
        guard_cmp->src1.size != HB_SIZE_64 || guard_cmp->src2.size != HB_SIZE_64)
        return false;
    scan_reg = guard_cmp->src1.reg;
    limit_reg = guard_cmp->src2.reg;
    if (scan_reg == limit_reg) return false;

    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.reg != scan_reg || add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (!direct_user_mem_allowed(buf, &body_cmp->src1) ||
        body_cmp->src1.size != HB_SIZE_8 ||
        body_cmp->src2.type != HB_OP_IMM || body_cmp->src2.imm != 0)
        return false;
    if (!mem_operand_uses_reg(&body_cmp->src1, scan_reg)) return false;
    if (mem_operand_uses_reg(&body_cmp->src1, limit_reg)) return false;

    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, scan_reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, limit_reg));

    loop_off = buf->size;
    emit_cmp_reg(buf, 23, 22);
    guard_exit_branch = emit_bcond_deferred(buf, arm64_cond(guard_jcc->cc));

    emit_add_imm(buf, 23, 23, 1);
    if (!emit_direct_mem_addr_with_override_scratch(buf, &body_cmp->src1, scan_reg, 23, 20))
        return false;
    emit_direct_mem_load_to_x20(buf, body_cmp->src1.size);
    emit_cmp_imm(buf, 20, 0);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(body_jcc->cc), loop_delta);

    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, scan_reg));
    emit_mov_imm_compact(buf, 21, 0);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, body_cmp->src1.size);
    emit_set_pc_imm64(buf, body_jcc->guest_addr + body_jcc->guest_len);
    body_done_branch = emit_b_deferred(buf);

    guard_exit_off = buf->size;
    patch_bcond(buf, guard_exit_branch, arm64_cond(guard_jcc->cc), guard_exit_off);
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, scan_reg));
    emit_mov_reg(buf, 20, 23);
    emit_mov_reg(buf, 21, 22);
    emit_sub_reg(buf, 22, 20, 21);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, guard_cmp->src1.size);
    emit_set_pc_imm64(buf, guard_jcc->target);

    done_off = buf->size;
    patch_b(buf, body_done_branch, done_off);
    return true;
}

static bool emit_store_count_loop_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t *store, *inc_count, *inc_ptr, *cmp, *jcc;
    hb_reg_t count_reg, ptr_reg;
    hb_size_t count_size;
    size_t loop_off;
    int32_t loop_delta;

    if (!jit_direct_mem_codegen_enabled(buf) || !block || block->instr_count != 5)
        return false;
    store = &block->instrs[0];
    inc_count = &block->instrs[1];
    inc_ptr = &block->instrs[2];
    cmp = &block->instrs[3];
    jcc = &block->instrs[4];

    if (store->op != HB_IR_STORE || inc_count->op != HB_IR_ADD ||
        inc_ptr->op != HB_IR_ADD || cmp->op != HB_IR_CMP || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->target != block->guest_addr) return false;
    if (jcc->cc != HB_CC_B && jcc->cc != HB_CC_NE) return false;
    if (!direct_user_mem_allowed(buf, &store->src1) || store->src1.size != HB_SIZE_8)
        return false;
    if (store->src2.type != HB_OP_REG && store->src2.type != HB_OP_IMM) return false;

    if (!is_plain_gpr_reg_operand(&inc_count->dst) ||
        !same_plain_gpr_operand(&inc_count->dst, &inc_count->src1) ||
        inc_count->src2.type != HB_OP_IMM || inc_count->src2.imm != 1)
        return false;
    if (inc_count->dst.size != HB_SIZE_32 && inc_count->dst.size != HB_SIZE_64) return false;
    if (!is_plain_gpr_reg_operand(&inc_ptr->dst) ||
        !same_plain_gpr_operand(&inc_ptr->dst, &inc_ptr->src1) ||
        inc_ptr->dst.size != HB_SIZE_64 ||
        inc_ptr->src2.type != HB_OP_IMM || inc_ptr->src2.imm != 1)
        return false;

    count_reg = inc_count->dst.reg;
    ptr_reg = inc_ptr->dst.reg;
    count_size = inc_count->dst.size;
    if (count_reg == ptr_reg) return false;
    if (mem_operand_uses_reg(&store->src1, count_reg)) return false;
    if (!mem_operand_uses_reg(&store->src1, ptr_reg)) return false;
    if (cmp->src1.type != HB_OP_REG || cmp->src1.reg != count_reg ||
        cmp->src1.size != count_size)
        return false;
    if (cmp->src2.type == HB_OP_REG) {
        if (!is_plain_gpr_reg_operand(&cmp->src2) || cmp->src2.reg == ptr_reg ||
            cmp->src2.reg == count_reg)
            return false;
    } else if (cmp->src2.type != HB_OP_IMM) {
        return false;
    }

    emit_ldr_x(buf, 22, 19, (uint32_t)reg_off(buf, count_reg));
    emit_mask_x_reg_to_size(buf, 22, 20, count_size);
    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, ptr_reg));

    loop_off = buf->size;
    if (!emit_direct_mem_addr_with_override_scratch(buf, &store->src1, ptr_reg, 23, 20))
        return false;
    if (store->src2.type == HB_OP_REG) {
        if (store->src2.reg == count_reg) {
            emit_mov_reg(buf, 20, 22);
        } else if (store->src2.reg == ptr_reg) {
            emit_mov_reg(buf, 20, 23);
        } else {
            hb_ir_operand_t sized = store->src2;
            sized.size = store->src1.size;
            if (!emit_load_gpr_sized_to_reg(buf, &sized, 20)) return false;
        }
    } else {
        emit_mov_imm64(buf, 20, (uint64_t)store->src2.imm);
    }
    emit_direct_mem_store_from_x20(buf, store->src1.size);

    emit_add_imm(buf, 22, 22, 1);
    emit_mask_x_reg_to_size(buf, 22, 20, count_size);
    emit_add_imm(buf, 23, 23, 1);

    if (cmp->src2.type == HB_OP_REG) {
        hb_ir_operand_t sized = cmp->src2;
        sized.size = count_size;
        if (!emit_load_gpr_sized_to_reg(buf, &sized, 21)) return false;
    } else {
        emit_mov_imm64(buf, 21, (uint64_t)cmp->src2.imm);
        emit_mask_x_reg_to_size(buf, 21, 20, count_size);
    }
    emit_sub_reg(buf, 20, 22, 21);
    emit_cmp_reg(buf, 22, 21);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(jcc->cc), loop_delta);

    emit_str_x(buf, 22, 19, (uint32_t)reg_off(buf, count_reg));
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, ptr_reg));
    emit_mov_reg(buf, 23, 20);
    emit_mov_reg(buf, 20, 22);
    emit_mov_reg(buf, 22, 23);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, count_size);
    emit_set_pc_imm64(buf, jcc->guest_addr + jcc->guest_len);
    return true;
}

static bool emit_zero_store_update_backedge_block(hb_codegen_buffer_t* buf,
                                                  const hb_ir_block_t* block) {
    const hb_ir_instr_t *zero, *store, *add_ptr2, *add_index, *sub_count, *jcc;
    if (!jit_direct_mem_codegen_enabled(buf) || !block || block->instr_count != 6)
        return false;

    zero = &block->instrs[0];
    store = &block->instrs[1];
    add_ptr2 = &block->instrs[2];
    add_index = &block->instrs[3];
    sub_count = &block->instrs[4];
    jcc = &block->instrs[5];

    if (zero->op != HB_IR_XOR || store->op != HB_IR_STORE ||
        add_ptr2->op != HB_IR_ADD || add_index->op != HB_IR_ADD ||
        sub_count->op != HB_IR_SUB || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!same_plain_gpr_operand(&zero->dst, &zero->src1) ||
        !same_plain_gpr_operand(&zero->dst, &zero->src2) ||
        zero->dst.size != HB_SIZE_8)
        return false;
    if (!direct_user_mem_allowed(buf, &store->src1) || store->src1.size != HB_SIZE_8 ||
        !same_plain_gpr_operand(&zero->dst, &store->src2))
        return false;
    if (!is_plain_gpr_reg_operand(&add_ptr2->dst) ||
        !same_plain_gpr_operand(&add_ptr2->dst, &add_ptr2->src1) ||
        add_ptr2->dst.size != HB_SIZE_64 ||
        add_ptr2->src2.type != HB_OP_IMM || add_ptr2->src2.imm != 2)
        return false;
    if (!is_plain_gpr_reg_operand(&add_index->dst) ||
        !same_plain_gpr_operand(&add_index->dst, &add_index->src1) ||
        add_index->dst.size != HB_SIZE_64 ||
        add_index->src2.type != HB_OP_IMM || add_index->src2.imm != 1)
        return false;
    if (!is_plain_gpr_reg_operand(&sub_count->dst) ||
        !same_plain_gpr_operand(&sub_count->dst, &sub_count->src1) ||
        sub_count->dst.size != HB_SIZE_64 ||
        sub_count->src2.type != HB_OP_IMM || sub_count->src2.imm != 1)
        return false;

    emit_store_zero_to_gpr_sized(buf, &zero->dst);
    uint32_t store_off = emit_direct_mem_addr_with_offset(buf, &store->src1);
    emit_direct_mem_store_zero_off(buf, store->src1.size, store_off);

    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, add_ptr2->dst.reg));
    emit_add_imm(buf, 23, 23, 2);
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, add_ptr2->dst.reg));

    emit_ldr_x(buf, 23, 19, (uint32_t)reg_off(buf, add_index->dst.reg));
    emit_add_imm(buf, 23, 23, 1);
    emit_str_x(buf, 23, 19, (uint32_t)reg_off(buf, add_index->dst.reg));

    emit_ldr_x(buf, 20, 19, (uint32_t)reg_off(buf, sub_count->dst.reg));
    emit_mov_imm_compact(buf, 21, 1);
    emit_subs_imm(buf, 22, 20, 1);
    emit_str_x(buf, 22, 19, (uint32_t)reg_off(buf, sub_count->dst.reg));
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_SUB, sub_count->dst.size);
    return emit_flags_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_direct_logic_rmw(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    hb_lazy_flags_kind_t kind;
    if (!instr || (instr->op != HB_IR_AND && instr->op != HB_IR_OR && instr->op != HB_IR_XOR))
        return false;
    if (!same_mem_operand(&instr->dst, &instr->src1)) return false;
    if (!direct_user_mem_allowed(buf, &instr->dst)) return false;
    if (instr->dst.size != HB_SIZE_8 && instr->dst.size != HB_SIZE_16 &&
        instr->dst.size != HB_SIZE_32 && instr->dst.size != HB_SIZE_64)
        return false;
    if (instr->src2.type == HB_OP_REG) {
        if (!is_gpr_reg_operand(&instr->src2)) return false;
    } else if (instr->src2.type != HB_OP_IMM) {
        return false;
    }
    if (!lazy_kind_for_scalar_op(instr->op, &kind)) return false;

    emit_direct_mem_addr(buf, &instr->dst);
    emit_mov_reg(buf, 0, 21);
    emit_direct_mem_load_to_x20(buf, instr->dst.size);

    if (instr->src2.type == HB_OP_REG) {
        hb_ir_operand_t sized = instr->src2;
        sized.size = instr->dst.size;
        if (!emit_load_gpr_sized_to_reg(buf, &sized, 21)) return false;
    } else {
        emit_mov_imm_compact(buf, 21, (uint64_t)instr->src2.imm);
        if (!imm_fits_size((uint64_t)instr->src2.imm, instr->dst.size))
            emit_mask_x_reg_to_size(buf, 21, 23, instr->dst.size);
    }

    switch (instr->op) {
        case HB_IR_AND: emit_and_reg(buf, 22, 20, 21); break;
        case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
        case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
        default: return false;
    }
    emit_mask_x_reg_to_size(buf, 22, 23, instr->dst.size);

    emit_mov_reg(buf, 21, 0);
    emit_mov_reg(buf, 20, 22);
    emit_direct_mem_store_from_x20(buf, instr->dst.size);

    emit_note_lazy_from_x20_x21_x22(buf, kind, instr->dst.size);
    return true;
}

static bool emit_direct_arith_rmw_impl(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr,
                                       bool record_flags) {
    hb_lazy_flags_kind_t kind = HB_LAZY_FLAGS_ADD;
    if (!instr || (instr->op != HB_IR_ADD && instr->op != HB_IR_SUB))
        return false;
    if (!same_mem_operand(&instr->dst, &instr->src1)) return false;
    if (!direct_user_mem_allowed(buf, &instr->dst)) return false;
    if (instr->dst.size != HB_SIZE_8 && instr->dst.size != HB_SIZE_16 &&
        instr->dst.size != HB_SIZE_32 && instr->dst.size != HB_SIZE_64)
        return false;
    if (instr->src2.type == HB_OP_REG) {
        if (!is_gpr_reg_operand(&instr->src2)) return false;
    } else if (instr->src2.type != HB_OP_IMM) {
        return false;
    }
    if (record_flags && !lazy_kind_for_scalar_op(instr->op, &kind)) return false;

    emit_direct_mem_addr(buf, &instr->dst);
    emit_mov_reg(buf, 0, 21);
    emit_direct_mem_load_to_x20(buf, instr->dst.size);

    if (instr->src2.type == HB_OP_REG) {
        hb_ir_operand_t sized = instr->src2;
        sized.size = instr->dst.size;
        if (!emit_load_gpr_sized_to_reg(buf, &sized, 21)) return false;
    } else {
        emit_mov_imm_compact(buf, 21, (uint64_t)instr->src2.imm);
        if (!imm_fits_size((uint64_t)instr->src2.imm, instr->dst.size))
            emit_mask_x_reg_to_size(buf, 21, 23, instr->dst.size);
    }

    switch (instr->op) {
        case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
        case HB_IR_SUB: emit_sub_reg(buf, 22, 20, 21); break;
        default: return false;
    }
    emit_mask_x_reg_to_size(buf, 22, 23, instr->dst.size);

    if (record_flags) {
        emit_mov_reg(buf, 9, 20);
        emit_mov_reg(buf, 10, 21);
        emit_mov_reg(buf, 21, 0);
        emit_mov_reg(buf, 20, 22);
        emit_direct_mem_store_from_x20(buf, instr->dst.size);
        emit_mov_reg(buf, 20, 9);
        emit_mov_reg(buf, 21, 10);
        emit_note_lazy_from_x20_x21_x22(buf, kind, instr->dst.size);
    } else {
        emit_mov_reg(buf, 21, 0);
        emit_mov_reg(buf, 20, 22);
        emit_direct_mem_store_from_x20(buf, instr->dst.size);
    }
    return true;
}

static bool emit_direct_arith_rmw(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    return emit_direct_arith_rmw_impl(buf, instr, true);
}

static bool emit_arith_rmw_dead_flags_test_jcc(hb_codegen_buffer_t* buf, const hb_ir_instr_t* rmw,
                                               const hb_ir_instr_t* test,
                                               const hb_ir_instr_t* jcc) {
    if (!rmw || (rmw->op != HB_IR_ADD && rmw->op != HB_IR_SUB)) return false;
    if (!test || test->op != HB_IR_TEST || !same_plain_gpr_operand(&test->src1, &test->src2))
        return false;
    if (!jcc || jcc->op != HB_IR_Jcc || (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE))
        return false;
    if (!emit_direct_arith_rmw_impl(buf, rmw, false)) return false;
    return emit_test_same_reg_jcc_pair(buf, test, jcc);
}

/* JIT helper declarations (implemented below) */
extern uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr);
extern void     hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                                 uint64_t dst_reg, uint64_t dst_size,
                                                 uint64_t dst_reg_offset);
extern void     hb_jit_helper_store_u64(hb_context_t* ctx, uint64_t addr, uint64_t val);
extern void     hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr, uint64_t val, uint64_t size);
extern void     hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_pop(hb_context_t* ctx);
extern void     hb_jit_helper_push(hb_context_t* ctx, uint64_t val);
extern void     hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta);
extern uint64_t hb_jit_helper_call(hb_context_t* ctx, uint64_t target, uint64_t ret_addr);
extern void     hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_xfg_dispatch_call(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_exec_binop_lazy(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                              uint64_t src1_reg, uint64_t src2_is_reg,
                                              uint64_t src2_value, uint64_t size);
extern void     hb_jit_helper_exec_cmp_test_lazy(hb_context_t* ctx, uint64_t op,
                                                  uint64_t src1_is_reg, uint64_t src1_value,
                                                  uint64_t src2_is_reg, uint64_t src2_value,
                                                  uint64_t size);
extern void     hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc);
extern void     hb_jit_helper_exec_setcc_lazy(hb_context_t* ctx, uint64_t cc,
                                              uint64_t dst_reg);
extern void     hb_jit_helper_exec_cmovcc_lazy(hb_context_t* ctx, uint64_t cc,
                                               uint64_t dst_reg, uint64_t src_is_reg,
                                               uint64_t src_value, uint64_t size);
extern void     hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_lahf(hb_context_t* ctx);
extern void     hb_jit_helper_sahf(hb_context_t* ctx);
extern void     hb_jit_helper_cpuid(hb_context_t* ctx);
extern void     hb_jit_helper_xgetbv(hb_context_t* ctx);
extern void     hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern hb_result_t hb_interpreter_exec_one_for_jit(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_atomic_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_two_block_loop(hb_context_t* ctx,
                                                  const hb_ir_block_t* first,
                                                  const hb_ir_block_t* second);
extern void     hb_jit_helper_exec_four_block_loop(hb_context_t* ctx,
                                                   const hb_ir_block_t* first,
                                                   const hb_ir_block_t* second,
                                                   const hb_ir_block_t* third,
                                                   const hb_ir_block_t* fourth);
extern void     hb_jit_helper_exec_ir_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_load_cmp_jcc_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_cmp_setcc_ret_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_i32_less_tiebreaker(hb_context_t* ctx,
                                                       const hb_ir_block_t* entry,
                                                       const hb_ir_block_t* equal,
                                                       const hb_ir_block_t* less);
extern void     hb_jit_helper_exec_unity_sort_inner_loop(hb_context_t* ctx,
                                                         const hb_ir_block_t* sort);
extern void     hb_jit_helper_exec_unity_string_bsearch_loop(hb_context_t* ctx,
                                                             const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_unity_freelist_fill_loop(hb_context_t* ctx,
                                                            const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_unity_u32_ptr_compare(hb_context_t* ctx,
                                                         const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_bsearch_loop(hb_context_t* ctx,
                                                               const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_string_hash(hb_context_t* ctx,
                                                     const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_string_equal(hb_context_t* ctx,
                                                      const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_rowptr_entry(hb_context_t* ctx,
                                                               const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_decode_row_loop(hb_context_t* ctx,
                                                                  const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_decode_row_entry(hb_context_t* ctx,
                                                                   const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_decode_col(hb_context_t* ctx,
                                                             const hb_ir_block_t* block);
extern void     hb_jit_helper_exec_mono_metadata_coded_index_search(hb_context_t* ctx,
                                                                     const hb_ir_block_t* block);

static void emit_call_helper(hb_codegen_buffer_t* buf, void* fn);

static bool operand_is_xmm_or_vecmem(const hb_ir_operand_t* op) {
    if (!op) return false;
    if (op->type == HB_OP_REG) return op->reg >= HB_REG_XMM0 && op->reg <= HB_REG_XMM15;
    return op->type == HB_OP_MEM && op->size == HB_SIZE_128;
}

static hb_result_t emit_interp_ir_helper(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_interp_ir);
    emit_return_if_helper_failed(buf);
    return HB_OK;
}

static hb_result_t emit_atomic_ir_helper(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    emit_dmb_ish(buf);
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_atomic_ir);
    emit_return_if_helper_failed(buf);
    emit_dmb_ish(buf);
    return HB_OK;
}

static bool block_has_atomic_ir(const hb_ir_block_t* block) {
    if (!block) return false;
    for (size_t i = 0; i < block->instr_count; i++) {
        /* MacRunner Lane A (2026-06-17): a LOCK-prefixed RMW (lock or/and/add/...) is a full
         * barrier too — treat it as atomic so the loop-optimizers (emit_two_block_loop_helper
         * etc., gated on this) don't bypass the per-instr DMB wrap and reintroduce the Mono
         * hazard-pointer livelock. */
        if (block->instrs[i].is_locked) return true;
        switch (block->instrs[i].op) {
            case HB_IR_CMPXCHG:
            case HB_IR_CMPXCHG8B:
            case HB_IR_XCHG:
            case HB_IR_XADD:
                return true;
            default:
                break;
        }
    }
    return false;
}

/* Emit a call to a C helper via BLR */
static void emit_call_helper(hb_codegen_buffer_t* buf, void* fn) {
    emit_mov_imm64(buf, 23, (uint64_t)fn);
    emit_blr(buf, 23);
}

static bool emit_two_block_loop_helper(hb_codegen_buffer_t* buf,
                                       const hb_ir_block_t* first,
                                       const hb_ir_block_t* second) {
    if (!buf || !first || !second || !first->instr_count || !second->instr_count)
        return false;
    if (block_has_atomic_ir(first) || block_has_atomic_ir(second))
        return false;
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)first);
    emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)second);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_two_block_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_four_block_loop_helper(hb_codegen_buffer_t* buf,
                                        const hb_ir_block_t* first,
                                        const hb_ir_block_t* second,
                                        const hb_ir_block_t* third,
                                        const hb_ir_block_t* fourth) {
    if (!buf || !first || !second || !third ||
        !first->instr_count || !second->instr_count || !third->instr_count)
        return false;
    if (block_has_atomic_ir(first) || block_has_atomic_ir(second) ||
        block_has_atomic_ir(third) || block_has_atomic_ir(fourth))
        return false;
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)first);
    emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)second);
    emit_mov_imm64(buf, 3, (uint64_t)(uintptr_t)third);
    emit_mov_imm64(buf, 4, (uint64_t)(uintptr_t)fourth);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_four_block_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_i32_less_tiebreaker_helper(hb_codegen_buffer_t* buf,
                                            const hb_ir_block_t* entry,
                                            const hb_ir_block_t* equal,
                                            const hb_ir_block_t* less) {
    if (!buf || !entry || !equal || !entry->instr_count || !equal->instr_count)
        return false;
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)entry);
    emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)equal);
    emit_mov_imm64(buf, 3, (uint64_t)(uintptr_t)less);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_i32_less_tiebreaker);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_unity_sort_inner_loop_helper(hb_codegen_buffer_t* buf,
                                              const hb_ir_block_t* sort) {
    if (!sort) return false;
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)sort);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_unity_sort_inner_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_xfg_dispatch_call_pair(hb_codegen_buffer_t* buf,
                                        const hb_ir_instr_t* mov,
                                        const hb_ir_instr_t* call) {
    if (!buf || !mov || !call) return false;
    if (mov->op != HB_IR_MOV || call->op != HB_IR_CALL || call->src1.type == HB_OP_NONE)
        return false;
    if (mov->dst.type != HB_OP_REG || mov->dst.reg != HB_REG_R10 ||
        mov->dst.size != HB_SIZE_64 || mov->dst.reg_offset != 0 ||
        mov->src1.type != HB_OP_IMM)
        return false;
    if (mov->guest_addr + mov->guest_len != call->guest_addr)
        return false;

    emit_mov_imm64(buf, 20, (uint64_t)mov->src1.imm);
    emit_str_x(buf, 20, 19, (uint32_t)reg_off(buf, HB_REG_R10));
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)call);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_xfg_dispatch_call);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool jit_exact_reg(const hb_ir_operand_t* op, hb_reg_t reg, hb_size_t size) {
    return is_plain_gpr_reg_operand(op) && op->reg == reg && op->size == size;
}

static bool jit_exact_mem(const hb_ir_operand_t* op, hb_reg_t base, hb_reg_t index,
                          uint8_t scale, int64_t disp, hb_size_t size) {
    return op && op->type == HB_OP_MEM && op->size == size &&
           op->mem.base == base && op->mem.index == index &&
           op->mem.scale == scale && op->mem.disp == disp &&
           op->mem.segment == 0 && !op->mem.addr32;
}

static bool unity_string_bsearch_loop_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 12) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_MOV &&
           jit_exact_reg(&i[0].dst, HB_REG_R10, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src1, HB_REG_R9, HB_SIZE_64) &&
           i[1].op == HB_IR_MOV &&
           jit_exact_reg(&i[1].dst, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[1].src1, HB_REG_RDI, HB_SIZE_64) &&
           i[2].op == HB_IR_SHR &&
           jit_exact_reg(&i[2].dst, HB_REG_R10, HB_SIZE_64) &&
           jit_exact_reg(&i[2].src1, HB_REG_R10, HB_SIZE_64) &&
           i[2].src2.type == HB_OP_IMM && i[2].src2.imm == 1 &&
           i[3].op == HB_IR_MOV &&
           jit_exact_reg(&i[3].dst, HB_REG_R11, HB_SIZE_64) &&
           jit_exact_reg(&i[3].src1, HB_REG_R10, HB_SIZE_64) &&
           i[4].op == HB_IR_ADD &&
           jit_exact_reg(&i[4].dst, HB_REG_R11, HB_SIZE_64) &&
           jit_exact_reg(&i[4].src1, HB_REG_R11, HB_SIZE_64) &&
           jit_exact_reg(&i[4].src2, HB_REG_R11, HB_SIZE_64) &&
           i[5].op == HB_IR_LOAD &&
           jit_exact_reg(&i[5].dst, HB_REG_RAX, HB_SIZE_64) &&
           jit_exact_mem(&i[5].src1, HB_REG_RBX, HB_REG_R11, 8, 0, HB_SIZE_64) &&
           i[6].op == HB_IR_SUB &&
           jit_exact_reg(&i[6].dst, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[6].src1, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[6].src2, HB_REG_RAX, HB_SIZE_64) &&
           i[7].op == HB_IR_NOP &&
           i[8].op == HB_IR_ZERO_EXTEND &&
           jit_exact_reg(&i[8].dst, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_mem(&i[8].src1, HB_REG_RAX, HB_REG_COUNT, 1, 0, HB_SIZE_8) &&
           i[9].op == HB_IR_ZERO_EXTEND &&
           jit_exact_reg(&i[9].dst, HB_REG_RCX, HB_SIZE_32) &&
           jit_exact_mem(&i[9].src1, HB_REG_RAX, HB_REG_R8, 1, 0, HB_SIZE_8) &&
           i[10].op == HB_IR_SUB &&
           jit_exact_reg(&i[10].dst, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_reg(&i[10].src1, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_reg(&i[10].src2, HB_REG_RCX, HB_SIZE_32) &&
           i[11].op == HB_IR_Jcc && i[11].cc == HB_CC_NE &&
	           i[11].target == block->guest_addr + 0x32;
}

static bool unity_u32_ptr_compare_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_LOAD &&
           jit_exact_reg(&i[0].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_mem(&i[0].src1, HB_REG_RDX, HB_REG_COUNT, 1, 0, HB_SIZE_32) &&
           i[1].op == HB_IR_CMP &&
           jit_exact_mem(&i[1].src1, HB_REG_RCX, HB_REG_COUNT, 1, 0, HB_SIZE_32) &&
           jit_exact_reg(&i[1].src2, HB_REG_RAX, HB_SIZE_32) &&
           i[2].op == HB_IR_Jcc && i[2].cc == HB_CC_NE &&
           i[2].target == block->guest_addr + 0x0d;
}

static bool unity_sort_inner_loop_candidate(const hb_ir_block_t* sort) {
    if (!sort || sort->instr_count != 7) return false;
    const hb_ir_instr_t* s = sort->instrs;

    return s[0].op == HB_IR_LOAD &&
           s[1].op == HB_IR_MOV &&
           s[2].op == HB_IR_STORE &&
           s[3].op == HB_IR_MOV &&
           s[4].op == HB_IR_LOAD &&
           s[5].op == HB_IR_SUB &&
           s[6].op == HB_IR_CALL &&
           jit_exact_reg(&s[6].src1, HB_REG_RBP, HB_SIZE_64);
}

static bool emit_unity_sort_inner_loop(hb_codegen_buffer_t* buf,
                                       const hb_ir_block_t* sort) {
    if (!unity_sort_inner_loop_candidate(sort)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=unity-sort-inner-codegen sort=%p\n",
                (void*)(uintptr_t)sort->guest_addr);
        fflush(stderr);
    }
    return emit_unity_sort_inner_loop_helper(buf, sort);
}

static bool mono_string_hash_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_XOR &&
           jit_exact_reg(&i[0].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[0].src1, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[0].src2, HB_REG_RAX, HB_SIZE_32) &&
           i[1].op == HB_IR_CMP &&
           jit_exact_mem(&i[1].src1, HB_REG_RCX, HB_REG_COUNT, 1, 0, HB_SIZE_8) &&
           jit_exact_reg(&i[1].src2, HB_REG_RAX, HB_SIZE_8) &&
           i[2].op == HB_IR_Jcc && i[2].cc == HB_CC_E &&
	           i[2].target == block->guest_addr + 0x24;
}

static bool mono_string_equal_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 2) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_CMP &&
           jit_exact_reg(&i[0].src1, HB_REG_RCX, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src2, HB_REG_RDX, HB_SIZE_64) &&
           i[1].op == HB_IR_Jcc && i[1].cc == HB_CC_E &&
           i[1].target == block->guest_addr + 0x2c;
}

static const hb_ir_instr_t* unity_freelist_fill_loop_instrs(const hb_ir_block_t* block,
                                                            size_t* prefix_count) {
    const hb_ir_instr_t* i;
    if (prefix_count) *prefix_count = 0;
    if (!block || !block->instr_count) return NULL;
    i = block->instrs;
    if (block->instr_count == 7 &&
        i[0].op == HB_IR_SIGN_EXTEND &&
        jit_exact_reg(&i[0].dst, HB_REG_R8, HB_SIZE_64) &&
        jit_exact_reg(&i[0].src1, HB_REG_R8, HB_SIZE_32) &&
        i[1].op == HB_IR_NOP) {
        if (prefix_count) *prefix_count = 2;
        return i + 2;
    }
    if (block->instr_count == 5) return i;
    return NULL;
}

static bool unity_freelist_fill_loop_candidate(const hb_ir_block_t* block) {
    size_t prefix_count = 0;
    const hb_ir_instr_t* i = unity_freelist_fill_loop_instrs(block, &prefix_count);
    if (!i) return false;

    return i[0].op == HB_IR_MOV &&
           jit_exact_reg(&i[0].dst, HB_REG_RAX, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src1, HB_REG_RCX, HB_SIZE_64) &&
           i[1].op == HB_IR_STORE &&
           jit_exact_mem(&i[1].src1, HB_REG_RCX, HB_REG_COUNT, 1, 0, HB_SIZE_64) &&
           jit_exact_reg(&i[1].src2, HB_REG_R14, HB_SIZE_64) &&
           i[2].op == HB_IR_XCHG &&
           jit_exact_mem(&i[2].src1, HB_REG_RSI, HB_REG_COUNT, 1, 0x80, HB_SIZE_64) &&
           jit_exact_reg(&i[2].src2, HB_REG_RAX, HB_SIZE_64) &&
           i[3].op == HB_IR_TEST &&
           jit_exact_reg(&i[3].src1, HB_REG_RAX, HB_SIZE_64) &&
           jit_exact_reg(&i[3].src2, HB_REG_RAX, HB_SIZE_64) &&
           i[4].op == HB_IR_Jcc && i[4].cc == HB_CC_E &&
           i[4].target == i[0].guest_addr + 0x17 &&
           (!prefix_count || i[0].guest_addr == block->guest_addr + 0x0c);
}

static const hb_ir_instr_t* mono_metadata_bsearch_loop_instrs(const hb_ir_block_t* block,
                                                              size_t* count) {
    const hb_ir_instr_t* i;
    size_t n;
    if (!block || !block->instr_count) return NULL;
    i = block->instrs;
    n = block->instr_count;
    if (n == 19 && i[0].op == HB_IR_NOP) {
        i++;
        n--;
    }
    if (count) *count = n;
    return i;
}

static bool mono_metadata_bsearch_loop_candidate(const hb_ir_block_t* block) {
    size_t n = 0;
    const hb_ir_instr_t* i = mono_metadata_bsearch_loop_instrs(block, &n);
    if (!i || n != 18) return false;

    return i[0].op == HB_IR_LEA &&
           jit_exact_reg(&i[0].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_mem(&i[0].src1, HB_REG_R9, HB_REG_RBP, 1, 0, HB_SIZE_32) &&
           i[1].op == HB_IR_CWD &&
           i[2].op == HB_IR_SUB &&
           jit_exact_reg(&i[2].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[2].src1, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[2].src2, HB_REG_RDX, HB_SIZE_32) &&
           i[3].op == HB_IR_SAR &&
           jit_exact_reg(&i[3].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[3].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[3].src2.type == HB_OP_IMM && i[3].src2.imm == 1 &&
           i[4].op == HB_IR_SIGN_EXTEND &&
           jit_exact_reg(&i[4].dst, HB_REG_RCX, HB_SIZE_64) &&
           jit_exact_reg(&i[4].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[5].op == HB_IR_LOAD &&
           jit_exact_reg(&i[5].dst, HB_REG_RCX, HB_SIZE_64) &&
           jit_exact_mem(&i[5].src1, HB_REG_RBX, HB_REG_RCX, 8, 24, HB_SIZE_64) &&
           i[6].op == HB_IR_SIGN_EXTEND &&
           jit_exact_reg(&i[6].dst, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_mem(&i[6].src1, HB_REG_RCX, HB_REG_COUNT, 1, 28, HB_SIZE_32) &&
           i[7].op == HB_IR_LOAD &&
           jit_exact_reg(&i[7].dst, HB_REG_RCX, HB_SIZE_64) &&
           jit_exact_mem(&i[7].src1, HB_REG_RCX, HB_REG_COUNT, 1, 16, HB_SIZE_64) &&
           i[8].op == HB_IR_ADD &&
           jit_exact_reg(&i[8].dst, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[8].src1, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[8].src2, HB_REG_RCX, HB_SIZE_64) &&
           i[9].op == HB_IR_MOV &&
           jit_exact_reg(&i[9].dst, HB_REG_RCX, HB_SIZE_32) &&
           jit_exact_reg(&i[9].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[10].op == HB_IR_CMP &&
           jit_exact_reg(&i[10].src1, HB_REG_R10, HB_SIZE_64) &&
           jit_exact_reg(&i[10].src2, HB_REG_R8, HB_SIZE_64) &&
           i[11].op == HB_IR_ADD &&
           jit_exact_reg(&i[11].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[11].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[11].src2.type == HB_OP_IMM && i[11].src2.imm == 1 &&
           i[12].op == HB_IR_CMOVcc && i[12].cc == HB_CC_AE &&
           jit_exact_reg(&i[12].dst, HB_REG_RCX, HB_SIZE_32) &&
           jit_exact_reg(&i[12].src1, HB_REG_R9, HB_SIZE_32) &&
           i[13].op == HB_IR_CMP &&
           jit_exact_reg(&i[13].src1, HB_REG_R10, HB_SIZE_64) &&
           jit_exact_reg(&i[13].src2, HB_REG_R8, HB_SIZE_64) &&
           i[14].op == HB_IR_MOV &&
           jit_exact_reg(&i[14].dst, HB_REG_R9, HB_SIZE_32) &&
           jit_exact_reg(&i[14].src1, HB_REG_RCX, HB_SIZE_32) &&
           i[15].op == HB_IR_CMOVcc && i[15].cc == HB_CC_AE &&
           jit_exact_reg(&i[15].dst, HB_REG_RBP, HB_SIZE_32) &&
           jit_exact_reg(&i[15].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[16].op == HB_IR_CMP &&
           jit_exact_reg(&i[16].src1, HB_REG_RBP, HB_SIZE_32) &&
           jit_exact_reg(&i[16].src2, HB_REG_RCX, HB_SIZE_32) &&
           i[17].op == HB_IR_Jcc && i[17].cc == HB_CC_L &&
           i[17].target == i[0].guest_addr;
}

static bool mono_metadata_decode_row_loop_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 9) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_MOV &&
           jit_exact_reg(&i[0].dst, HB_REG_RCX, HB_SIZE_32) &&
           jit_exact_reg(&i[0].src1, HB_REG_RDX, HB_SIZE_32) &&
           i[1].op == HB_IR_NOP &&
           i[2].op == HB_IR_MOV &&
           jit_exact_reg(&i[2].dst, HB_REG_R8, HB_SIZE_32) &&
           jit_exact_reg(&i[2].src1, HB_REG_R15, HB_SIZE_32) &&
           i[3].op == HB_IR_SHR &&
           jit_exact_reg(&i[3].dst, HB_REG_R8, HB_SIZE_32) &&
           jit_exact_reg(&i[3].src1, HB_REG_R8, HB_SIZE_32) &&
           jit_exact_reg(&i[3].src2, HB_REG_RCX, HB_SIZE_8) &&
           i[4].op == HB_IR_AND &&
           jit_exact_reg(&i[4].dst, HB_REG_R8, HB_SIZE_32) &&
           jit_exact_reg(&i[4].src1, HB_REG_R8, HB_SIZE_32) &&
           i[4].src2.type == HB_OP_IMM && i[4].src2.imm == 3 &&
           i[5].op == HB_IR_ADD &&
           jit_exact_reg(&i[5].dst, HB_REG_R8, HB_SIZE_32) &&
           jit_exact_reg(&i[5].src1, HB_REG_R8, HB_SIZE_32) &&
           i[5].src2.type == HB_OP_IMM && i[5].src2.imm == 1 &&
           i[6].op == HB_IR_MOV &&
           jit_exact_reg(&i[6].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[6].src1, HB_REG_R8, HB_SIZE_32) &&
           i[7].op == HB_IR_SUB &&
           jit_exact_reg(&i[7].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[7].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[7].src2.type == HB_OP_IMM && i[7].src2.imm == 1 &&
           i[8].op == HB_IR_Jcc && i[8].cc == HB_CC_E &&
	           i[8].target == block->guest_addr + 0x34;
}

static bool mono_metadata_rowptr_entry_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 9) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_STORE &&
           jit_exact_mem(&i[0].src1, HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src2, HB_REG_RBX, HB_SIZE_64) &&
           i[1].op == HB_IR_STORE &&
           jit_exact_mem(&i[1].src1, HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64) &&
           jit_exact_reg(&i[1].src2, HB_REG_RSI, HB_SIZE_64) &&
           i[2].op == HB_IR_PUSH &&
           jit_exact_reg(&i[2].src1, HB_REG_RDI, HB_SIZE_64) &&
           i[3].op == HB_IR_SUB &&
           jit_exact_reg(&i[3].dst, HB_REG_RSP, HB_SIZE_64) &&
           jit_exact_reg(&i[3].src1, HB_REG_RSP, HB_SIZE_64) &&
           i[3].src2.type == HB_OP_IMM && i[3].src2.imm == 64 &&
           i[4].op == HB_IR_MOV &&
           jit_exact_reg(&i[4].dst, HB_REG_RDI, HB_SIZE_64) &&
           jit_exact_reg(&i[4].src1, HB_REG_RCX, HB_SIZE_64) &&
           i[5].op == HB_IR_MOV &&
           jit_exact_reg(&i[5].dst, HB_REG_RBX, HB_SIZE_32) &&
           jit_exact_reg(&i[5].src1, HB_REG_RDX, HB_SIZE_32) &&
           i[6].op == HB_IR_LOAD &&
           jit_exact_reg(&i[6].dst, HB_REG_RCX, HB_SIZE_32) &&
           jit_exact_mem(&i[6].src1, HB_REG_RCX, HB_REG_COUNT, 1, 0x80, HB_SIZE_32) &&
           i[7].op == HB_IR_CMP &&
           jit_exact_reg(&i[7].src1, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_reg(&i[7].src2, HB_REG_RCX, HB_SIZE_32) &&
           i[8].op == HB_IR_Jcc && i[8].cc == HB_CC_B &&
           i[8].target == block->guest_addr + 0xb6;
}

static bool mono_metadata_decode_row_entry_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 18) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_STORE &&
           jit_exact_mem(&i[0].src1, HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src2, HB_REG_RBX, HB_SIZE_64) &&
           i[1].op == HB_IR_STORE &&
           jit_exact_mem(&i[1].src1, HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64) &&
           jit_exact_reg(&i[1].src2, HB_REG_RBP, HB_SIZE_64) &&
           i[2].op == HB_IR_STORE &&
           jit_exact_mem(&i[2].src1, HB_REG_RSP, HB_REG_COUNT, 1, 24, HB_SIZE_64) &&
           jit_exact_reg(&i[2].src2, HB_REG_RSI, HB_SIZE_64) &&
           i[3].op == HB_IR_PUSH &&
           jit_exact_reg(&i[3].src1, HB_REG_RDI, HB_SIZE_64) &&
           i[4].op == HB_IR_PUSH &&
           jit_exact_reg(&i[4].src1, HB_REG_R14, HB_SIZE_64) &&
           i[5].op == HB_IR_PUSH &&
           jit_exact_reg(&i[5].src1, HB_REG_R15, HB_SIZE_64) &&
           i[6].op == HB_IR_SUB &&
           jit_exact_reg(&i[6].dst, HB_REG_RSP, HB_SIZE_64) &&
           jit_exact_reg(&i[6].src1, HB_REG_RSP, HB_SIZE_64) &&
           i[6].src2.type == HB_OP_IMM && i[6].src2.imm == 32 &&
           i[7].op == HB_IR_LOAD &&
           jit_exact_reg(&i[7].dst, HB_REG_R15, HB_SIZE_32) &&
           jit_exact_mem(&i[7].src1, HB_REG_RCX, HB_REG_COUNT, 1, 12, HB_SIZE_32) &&
           i[8].op == HB_IR_MOV &&
           jit_exact_reg(&i[8].dst, HB_REG_RBP, HB_SIZE_32) &&
           jit_exact_reg(&i[8].src1, HB_REG_R9, HB_SIZE_32) &&
           i[9].op == HB_IR_LOAD &&
           jit_exact_reg(&i[9].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_mem(&i[9].src1, HB_REG_RCX, HB_REG_COUNT, 1, 8, HB_SIZE_32) &&
           i[10].op == HB_IR_MOV &&
           jit_exact_reg(&i[10].dst, HB_REG_RDI, HB_SIZE_32) &&
           jit_exact_reg(&i[10].src1, HB_REG_R15, HB_SIZE_32) &&
           i[11].op == HB_IR_SHR &&
           jit_exact_reg(&i[11].dst, HB_REG_RDI, HB_SIZE_32) &&
           jit_exact_reg(&i[11].src1, HB_REG_RDI, HB_SIZE_32) &&
           i[11].src2.type == HB_OP_IMM && i[11].src2.imm == 24 &&
           i[12].op == HB_IR_AND &&
           jit_exact_reg(&i[12].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[12].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[12].src2.type == HB_OP_IMM && i[12].src2.imm == 0xffffff &&
           i[13].op == HB_IR_MOV &&
           jit_exact_reg(&i[13].dst, HB_REG_R14, HB_SIZE_64) &&
           jit_exact_reg(&i[13].src1, HB_REG_R8, HB_SIZE_64) &&
           i[14].op == HB_IR_MOV &&
           jit_exact_reg(&i[14].dst, HB_REG_RBX, HB_SIZE_32) &&
           jit_exact_reg(&i[14].src1, HB_REG_RDX, HB_SIZE_32) &&
           i[15].op == HB_IR_MOV &&
           jit_exact_reg(&i[15].dst, HB_REG_RSI, HB_SIZE_64) &&
           jit_exact_reg(&i[15].src1, HB_REG_RCX, HB_SIZE_64) &&
           i[16].op == HB_IR_CMP &&
           jit_exact_reg(&i[16].src1, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_reg(&i[16].src2, HB_REG_RAX, HB_SIZE_32) &&
           i[17].op == HB_IR_Jcc && i[17].cc == HB_CC_B &&
           i[17].target == block->guest_addr + 0x59;
}

static bool mono_metadata_decode_col_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 13) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_STORE &&
           jit_exact_mem(&i[0].src1, HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64) &&
           jit_exact_reg(&i[0].src2, HB_REG_RBX, HB_SIZE_64) &&
           i[1].op == HB_IR_STORE &&
           jit_exact_mem(&i[1].src1, HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64) &&
           jit_exact_reg(&i[1].src2, HB_REG_RBP, HB_SIZE_64) &&
           i[2].op == HB_IR_STORE &&
           jit_exact_mem(&i[2].src1, HB_REG_RSP, HB_REG_COUNT, 1, 24, HB_SIZE_64) &&
           jit_exact_reg(&i[2].src2, HB_REG_RSI, HB_SIZE_64) &&
           i[3].op == HB_IR_PUSH &&
           jit_exact_reg(&i[3].src1, HB_REG_RDI, HB_SIZE_64) &&
           i[4].op == HB_IR_SUB &&
           jit_exact_reg(&i[4].dst, HB_REG_RSP, HB_SIZE_64) &&
           jit_exact_reg(&i[4].src1, HB_REG_RSP, HB_SIZE_64) &&
           i[4].src2.type == HB_OP_IMM && i[4].src2.imm == 32 &&
           i[5].op == HB_IR_LOAD &&
           jit_exact_reg(&i[5].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_mem(&i[5].src1, HB_REG_RCX, HB_REG_COUNT, 1, 8, HB_SIZE_32) &&
           i[6].op == HB_IR_MOV &&
           jit_exact_reg(&i[6].dst, HB_REG_RSI, HB_SIZE_32) &&
           jit_exact_reg(&i[6].src1, HB_REG_R8, HB_SIZE_32) &&
           i[7].op == HB_IR_LOAD &&
           jit_exact_reg(&i[7].dst, HB_REG_RBX, HB_SIZE_32) &&
           jit_exact_mem(&i[7].src1, HB_REG_RCX, HB_REG_COUNT, 1, 12, HB_SIZE_32) &&
           i[8].op == HB_IR_AND &&
           jit_exact_reg(&i[8].dst, HB_REG_RAX, HB_SIZE_32) &&
           jit_exact_reg(&i[8].src1, HB_REG_RAX, HB_SIZE_32) &&
           i[8].src2.type == HB_OP_IMM && i[8].src2.imm == 0xffffff &&
           i[9].op == HB_IR_MOV &&
           jit_exact_reg(&i[9].dst, HB_REG_RBP, HB_SIZE_32) &&
           jit_exact_reg(&i[9].src1, HB_REG_RDX, HB_SIZE_32) &&
           i[10].op == HB_IR_MOV &&
           jit_exact_reg(&i[10].dst, HB_REG_RDI, HB_SIZE_64) &&
           jit_exact_reg(&i[10].src1, HB_REG_RCX, HB_SIZE_64) &&
           i[11].op == HB_IR_CMP &&
           jit_exact_reg(&i[11].src1, HB_REG_RDX, HB_SIZE_32) &&
           jit_exact_reg(&i[11].src2, HB_REG_RAX, HB_SIZE_32) &&
           i[12].op == HB_IR_Jcc && i[12].cc == HB_CC_B &&
           i[12].target == block->guest_addr + 0x4b;
}

static bool mono_metadata_coded_index_search_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 9) return false;
    const hb_ir_instr_t* i = block->instrs;

    return i[0].op == HB_IR_PUSH &&
           jit_exact_reg(&i[0].src1, HB_REG_RBX, HB_SIZE_64) &&
           i[1].op == HB_IR_PUSH &&
           jit_exact_reg(&i[1].src1, HB_REG_RDI, HB_SIZE_64) &&
           i[2].op == HB_IR_PUSH &&
           jit_exact_reg(&i[2].src1, HB_REG_R14, HB_SIZE_64) &&
           i[3].op == HB_IR_SUB &&
           jit_exact_reg(&i[3].dst, HB_REG_RSP, HB_SIZE_64) &&
           jit_exact_reg(&i[3].src1, HB_REG_RSP, HB_SIZE_64) &&
           i[3].src2.type == HB_OP_IMM && i[3].src2.imm == 48 &&
           i[4].op == HB_IR_LEA &&
           jit_exact_reg(&i[4].dst, HB_REG_R14, HB_SIZE_64) &&
           jit_exact_mem(&i[4].src1, HB_REG_RCX, HB_REG_COUNT, 1, 0x390, HB_SIZE_64) &&
           i[5].op == HB_IR_MOV &&
           jit_exact_reg(&i[5].dst, HB_REG_RBX, HB_SIZE_64) &&
           jit_exact_reg(&i[5].src1, HB_REG_R8, HB_SIZE_64) &&
           i[6].op == HB_IR_MOV &&
           jit_exact_reg(&i[6].dst, HB_REG_RDI, HB_SIZE_32) &&
           jit_exact_reg(&i[6].src1, HB_REG_RDX, HB_SIZE_32) &&
           i[7].op == HB_IR_TEST &&
           jit_exact_reg(&i[7].src1, HB_REG_R8, HB_SIZE_64) &&
           jit_exact_reg(&i[7].src2, HB_REG_R8, HB_SIZE_64) &&
           i[8].op == HB_IR_Jcc && i[8].cc == HB_CC_NE &&
           i[8].target == block->guest_addr + 0x3a;
}

static bool emit_unity_string_bsearch_loop(hb_codegen_buffer_t* buf,
                                           const hb_ir_block_t* block) {
    if (!unity_string_bsearch_loop_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=unity-string-bsearch block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_unity_string_bsearch_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_unity_freelist_fill_loop(hb_codegen_buffer_t* buf,
                                          const hb_ir_block_t* block) {
    if (!unity_freelist_fill_loop_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=unity-freelist-fill-loop block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_unity_freelist_fill_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_unity_u32_ptr_compare(hb_codegen_buffer_t* buf,
                                       const hb_ir_block_t* block) {
    if (!unity_u32_ptr_compare_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=unity-u32-ptr-compare block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_unity_u32_ptr_compare);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_string_hash(hb_codegen_buffer_t* buf,
                                  const hb_ir_block_t* block) {
    if (!mono_string_hash_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-string-hash block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_string_hash);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_string_equal(hb_codegen_buffer_t* buf,
                                   const hb_ir_block_t* block) {
    if (!mono_string_equal_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-string-equal block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_string_equal);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool mono_metadata_fusions_disabled(void) {
    const char* env = getenv("MACRUNNER_HB_DISABLE_MONO_METADATA_FUSIONS");
    return env && *env && *env != '0';
}

static bool emit_mono_metadata_bsearch_loop(hb_codegen_buffer_t* buf,
                                            const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    if (!mono_metadata_bsearch_loop_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-bsearch block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_bsearch_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_metadata_rowptr_entry(hb_codegen_buffer_t* buf,
                                            const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    if (!mono_metadata_rowptr_entry_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-rowptr-entry block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_rowptr_entry);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_metadata_decode_row_loop(hb_codegen_buffer_t* buf,
                                               const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    if (!mono_metadata_decode_row_loop_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-decode-row block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_decode_row_loop);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_metadata_decode_row_entry(hb_codegen_buffer_t* buf,
                                                const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    if (!mono_metadata_decode_row_entry_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-decode-row-entry block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_decode_row_entry);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_metadata_decode_col(hb_codegen_buffer_t* buf,
                                          const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    /*
     * Hollow Knight/Unity Mono mini_init trips System.RuntimeType vtable slot
     * validation when this helper fuses mono_metadata_decode_row_col
     * (mono-2.0-bdwgc.dll RVA 0x184130). Keep the rest of the metadata
     * fast paths enabled while this column decoder falls back to normal IR.
     */
    return false;
    if (!mono_metadata_decode_col_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-decode-col block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_decode_col);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool emit_mono_metadata_coded_index_search(hb_codegen_buffer_t* buf,
                                                  const hb_ir_block_t* block) {
    if (mono_metadata_fusions_disabled()) return false;
    if (!mono_metadata_coded_index_search_candidate(block)) return false;
    const char* trace = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
    if (trace && *trace && *trace != '0') {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=mono-metadata-coded-index-search block=%p\n",
                (void*)(uintptr_t)block->guest_addr);
        fflush(stderr);
    }
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_mono_metadata_coded_index_search);
    emit_return_if_helper_failed(buf);
    return true;
}

static bool hot_helper_block_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* a = &block->instrs[0];
    const hb_ir_instr_t* b = &block->instrs[1];
    const hb_ir_instr_t* c = &block->instrs[2];
    if (a->op == HB_IR_LOAD && (b->op == HB_IR_CMP || b->op == HB_IR_TEST) &&
        c->op == HB_IR_Jcc)
        return true;
    if ((a->op == HB_IR_CMP || a->op == HB_IR_TEST) &&
        b->op == HB_IR_SETcc && c->op == HB_IR_RET)
        return true;
    return false;
}

static bool hot_helper_scalar_operand(const hb_ir_operand_t* op, bool allow_imm) {
    if (!op) return false;
    if (op->type == HB_OP_IMM) return allow_imm;
    if (op->type == HB_OP_REG)
        return op->reg < HB_REG_XMM0 && op->size && op->size <= HB_SIZE_64;
    if (op->type == HB_OP_MEM)
        return op->size && op->size <= HB_SIZE_64 && op->mem.base != HB_REG_RIP &&
               !op->mem.addr32;
    return false;
}

static bool hot_helper_load_cmp_jcc_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* cmp = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    return load->op == HB_IR_LOAD &&
           load->dst.type == HB_OP_REG && load->dst.reg < HB_REG_XMM0 &&
           load->dst.size && load->dst.size <= HB_SIZE_64 &&
           load->src1.type == HB_OP_MEM && hot_helper_scalar_operand(&load->src1, false) &&
           (cmp->op == HB_IR_CMP || cmp->op == HB_IR_TEST) &&
           hot_helper_scalar_operand(&cmp->src1, true) &&
           hot_helper_scalar_operand(&cmp->src2, true) &&
           jcc->op == HB_IR_Jcc;
}

static bool hot_helper_cmp_setcc_ret_candidate(const hb_ir_block_t* block) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* cmp = &block->instrs[0];
    const hb_ir_instr_t* setcc = &block->instrs[1];
    const hb_ir_instr_t* ret = &block->instrs[2];
    return (cmp->op == HB_IR_CMP || cmp->op == HB_IR_TEST) &&
           hot_helper_scalar_operand(&cmp->src1, true) &&
           hot_helper_scalar_operand(&cmp->src2, true) &&
           setcc->op == HB_IR_SETcc &&
           setcc->dst.size == HB_SIZE_8 &&
           (setcc->dst.type == HB_OP_REG || setcc->dst.type == HB_OP_MEM) &&
           hot_helper_scalar_operand(&setcc->dst, false) &&
           ret->op == HB_IR_RET;
}

static bool emit_hot_helper_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    if (!hot_helper_block_candidate(block)) return false;
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)block);
    if (hot_helper_load_cmp_jcc_candidate(block))
        emit_call_helper(buf, (void*)hb_jit_helper_exec_load_cmp_jcc_block);
    else if (hot_helper_cmp_setcc_ret_candidate(block))
        emit_call_helper(buf, (void*)hb_jit_helper_exec_cmp_setcc_ret_block);
    else
        emit_call_helper(buf, (void*)hb_jit_helper_exec_ir_block);
    emit_return_if_helper_failed(buf);
    return true;
}

/* --- Per-instruction codegen --- */
static hb_result_t codegen_instr(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    switch (instr->op) {
        case HB_IR_NOP:
            emit_nop(buf);
            return HB_OK;

        case HB_IR_MOV:
            if (emit_native_xmm_mov(buf, instr))
                return HB_OK;
            if (operand_is_xmm_or_vecmem(&instr->dst) || operand_is_xmm_or_vecmem(&instr->src1))
                return emit_interp_ir_helper(buf, instr);
            if (emit_native_scalar_mov(buf, instr))
                return HB_OK;
            if (instr->src1.type == HB_OP_MEM) {
                if (is_gpr_reg_operand(&instr->dst) &&
                    direct_user_mem_allowed(buf, &instr->src1)) {
                    emit_direct_mem_addr_for_instr(buf, &instr->src1, instr);
                    emit_mov_reg(buf, 0, 19);
                    emit_mov_reg(buf, 1, 21);
                    emit_mov_imm_compact(buf, 2, (uint64_t)instr->dst.reg);
                    emit_mov_imm_compact(buf, 3, (uint64_t)instr->dst.size);
                    emit_mov_imm_compact(buf, 4, (uint64_t)instr->dst.reg_offset);
                    emit_call_helper(buf, (void*)hb_jit_helper_load_to_reg_sized);
                    emit_return_if_helper_failed(buf);
                    return HB_OK;
                }
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_mov_operand_lazy);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            if (!is_gpr_reg_operand(&instr->dst) ||
                (instr->src1.type == HB_OP_REG && !is_gpr_reg_operand(&instr->src1)) ||
                instr->dst.size != HB_SIZE_64 ||
                (instr->src1.type == HB_OP_REG && instr->src1.size != HB_SIZE_64)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_mov_operand_lazy);
                return HB_OK;
            }
            emit_load_operand(buf, &instr->src1);
            emit_store_operand(buf, &instr->dst);
            return HB_OK;

        case HB_IR_ZERO_EXTEND:
        case HB_IR_SIGN_EXTEND:
            if (emit_native_extend(buf, instr))
                return HB_OK;
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_extend_operand_lazy);
            return HB_OK;

        case HB_IR_LEA: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = 0;
            if (instr->src1.mem.base < HB_REG_COUNT) {
                if (instr->src1.mem.base == HB_REG_RIP) {
                    addr = instr->guest_addr + instr->guest_len;
                } else {
                    size_t off = reg_off(buf, instr->src1.mem.base);
                    emit_ldr_gpr(buf, 20, 19, (uint32_t)off);
                    addr = 0; /* loaded into X20 */
                }
            } else {
                emit_mov_imm64(buf, 20, 0);
            }
            if (instr->src1.mem.base == HB_REG_RIP) {
                emit_mov_imm64(buf, 20, addr);
            }
            if (instr->src1.mem.index < HB_REG_COUNT) {
                size_t off = reg_off(buf, instr->src1.mem.index);
                emit_ldr_gpr(buf, 21, 19, (uint32_t)off);
                /* ADD X20, X20, X21, LSL #scale */
                uint32_t shift = (instr->src1.mem.scale == 1) ? 0 :
                                 (instr->src1.mem.scale == 2) ? 1 :
                                 (instr->src1.mem.scale == 4) ? 2 :
                                 (instr->src1.mem.scale == 8) ? 3 : 0;
                emit_u32(buf, 0x8b000000 | (21 << 16) | (shift << 10) | (20 << 5) | 20);
            }
            if (instr->src1.mem.disp != 0) {
                if (instr->src1.mem.disp >= 0 && instr->src1.mem.disp < 4096) {
                    emit_add_imm(buf, 20, 20, (uint32_t)instr->src1.mem.disp);
                } else if (instr->src1.mem.disp < 0 && -instr->src1.mem.disp < 4096) {
                    emit_sub_imm(buf, 20, 20, (uint32_t)(-instr->src1.mem.disp));
                } else {
                    emit_mov_imm64(buf, 21, (uint64_t)instr->src1.mem.disp);
                    emit_add_reg(buf, 20, 20, 21);
                }
            }
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_ADD:
        case HB_IR_ADC:
        case HB_IR_SUB:
        case HB_IR_SBB:
        case HB_IR_AND:
        case HB_IR_OR:
        case HB_IR_XOR: {
            hb_lazy_flags_kind_t kind;
            if (emit_direct_arith_rmw(buf, instr)) return HB_OK;
            if (emit_direct_logic_rmw(buf, instr)) return HB_OK;
            if (!is_plain_gpr_reg_operand(&instr->dst) ||
                !is_plain_gpr_reg_operand(&instr->src1) ||
                (instr->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&instr->src2)) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_operand_lazy);
                return HB_OK;
            }
            if (instr->op == HB_IR_ADC || instr->op == HB_IR_SBB) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->op);
                emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
                emit_mov_imm64(buf, 3, (uint64_t)instr->src1.reg);
                emit_mov_imm64(buf, 4, instr->src2.type == HB_OP_REG ? 1ULL : 0ULL);
                emit_mov_imm64(buf, 5, instr->src2.type == HB_OP_REG ? (uint64_t)instr->src2.reg : (uint64_t)instr->src2.imm);
                emit_mov_imm64(buf, 6, (uint64_t)instr->dst.size);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_lazy);
                return HB_OK;
            }
            if (!lazy_kind_for_scalar_op(instr->op, &kind)) return HB_ERR_INTERNAL;
            if (!emit_load_gpr_sized_to_reg(buf, &instr->src1, 20)) return HB_ERR_INTERNAL;
            if (instr->src2.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src2, 21)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm_compact(buf, 21, (uint64_t)instr->src2.imm);
                emit_mask_x_reg_to_size(buf, 21, 23, instr->dst.size);
            }
            switch (instr->op) {
                case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
                case HB_IR_SUB: emit_sub_reg(buf, 22, 20, 21); break;
                case HB_IR_AND: emit_and_reg(buf, 22, 20, 21); break;
                case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
                case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
                default: return HB_ERR_INTERNAL;
            }
            emit_mask_x_reg_to_size(buf, 22, 23, instr->dst.size);
            emit_note_lazy_from_x20_x21_x22(buf, kind, instr->dst.size);
            emit_mov_reg(buf, 20, 22);
            emit_store_x20_to_gpr_sized(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_MUL:
        case HB_IR_IMUL:
        case HB_IR_DIV:
        case HB_IR_IDIV: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_mul_div_operand);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR:
        case HB_IR_ROL:
        case HB_IR_ROR: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG)
                return emit_interp_ir_helper(buf, instr);
            if (!is_plain_gpr_reg_operand(&instr->dst) ||
                !is_plain_gpr_reg_operand(&instr->src1) ||
                (instr->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&instr->src2))) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_operand_lazy);
                return HB_OK;
            }
            uint64_t src2_type = 0;
            uint64_t src2_value = 0;
            if (instr->src2.type == HB_OP_REG) {
                src2_type = 1;
                src2_value = (uint64_t)instr->src2.reg;
            } else if (instr->src2.type == HB_OP_IMM) {
                src2_type = 0;
                src2_value = (uint64_t)instr->src2.imm;
            } else {
                return HB_ERR_INTERNAL;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->op);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_mov_imm64(buf, 3, (uint64_t)instr->src1.reg);
            emit_mov_imm64(buf, 4, src2_type);
            emit_mov_imm64(buf, 5, src2_value);
            emit_mov_imm64(buf, 6, (uint64_t)instr->dst.size);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_lazy);
            return HB_OK;
        }

        case HB_IR_SHLD:
        case HB_IR_SHRD: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_double_shift_operand);
            return HB_OK;
        }

        case HB_IR_NOT: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_not_operand_lazy);
            return HB_OK;
        }

        case HB_IR_NEG: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_neg_operand_lazy);
            return HB_OK;
        }

        case HB_IR_LAHF: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_lahf);
            return HB_OK;
        }

        case HB_IR_SAHF: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_sahf);
            return HB_OK;
        }

        case HB_IR_CPUID: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_cpuid);
            return HB_OK;
        }

        case HB_IR_XGETBV: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_xgetbv);
            return HB_OK;
        }

        case HB_IR_CMP:
        case HB_IR_TEST: {
            hb_lazy_flags_kind_t kind;
            if ((instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM) ||
                (instr->src1.type == HB_OP_REG && instr->src1.reg_offset != 0) ||
                (instr->src2.type == HB_OP_REG && instr->src2.reg_offset != 0)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_cmp_test_operand_lazy);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            if (!lazy_kind_for_scalar_op(instr->op, &kind)) return HB_ERR_INTERNAL;
            if (instr->src1.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src1, 20)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm64(buf, 20, (uint64_t)instr->src1.imm);
                emit_mask_x_reg_to_size(buf, 20, 23, instr->src1.size);
            }
            if (instr->src2.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src2, 21)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm64(buf, 21, (uint64_t)instr->src2.imm);
                emit_mask_x_reg_to_size(buf, 21, 23, instr->src1.size);
            }
            if (instr->op == HB_IR_TEST) emit_and_reg(buf, 22, 20, 21);
            else emit_sub_reg(buf, 22, 20, 21);
            emit_mask_x_reg_to_size(buf, 22, 23, instr->src1.size);
            emit_note_lazy_from_x20_x21_x22(buf, kind, instr->src1.size);
            return HB_OK;
        }

        case HB_IR_FENCE:
            emit_guest_fence(buf, (hb_fence_kind_t)instr->src1.imm);
            return HB_OK;

        case HB_IR_LOAD: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            if (is_xmm_reg_operand(&instr->dst) && jit_direct_mem_codegen_enabled(buf) &&
                direct_user_xmm_mem_allowed(buf, &instr->src1)) {
                emit_direct_mem_addr(buf, &instr->src1);
                emit_direct_mem128_load_to_x20_x22(buf);
                emit_store_x20_x22_to_xmm(buf, instr->dst.reg);
                return HB_OK;
            }
            if (!is_gpr_reg_operand(&instr->dst))
                return emit_interp_ir_helper(buf, instr);
            if (jit_direct_mem_codegen_enabled(buf) &&
                direct_user_mem_allowed(buf, &instr->src1) &&
                !wide_self_base_load_needs_helper(instr)) {
                if (!emit_direct_mem_load_to_gpr_tso(buf, &instr->src1, &instr->dst))
                    return HB_ERR_INTERNAL;
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_load_operand_lazy);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_STORE: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            if (jit_direct_mem_codegen_enabled(buf) &&
                direct_user_xmm_mem_allowed(buf, &instr->src1) &&
                is_xmm_reg_operand(&instr->src2)) {
                emit_load_xmm_to_x20_x22(buf, instr->src2.reg);
                /* emit_direct_mem_addr clobbers x22 (index/large-disp) which holds the HIGH 64
                 * bits of the XMM value; preserve it across the address computation, else an
                 * indexed SSE store corrupts the high qword (the HK realloc-copy corruption). */
                if (direct_mem_addr_preserves_x22(&instr->src1)) {
                    emit_direct_mem_addr(buf, &instr->src1);
                } else {
                    emit_mov_reg(buf, 23, 22);
                    emit_direct_mem_addr(buf, &instr->src1);
                    emit_mov_reg(buf, 22, 23);
                }
                emit_direct_mem128_store_from_x20_x22(buf);
                return HB_OK;
            }
            if (instr->src2.type == HB_OP_REG && !is_gpr_reg_operand(&instr->src2))
                return emit_interp_ir_helper(buf, instr);
            if (jit_direct_mem_codegen_enabled(buf) &&
                direct_user_mem_allowed(buf, &instr->src1) &&
                (instr->src2.type == HB_OP_REG || instr->src2.type == HB_OP_IMM)) {
                if (instr->src2.type == HB_OP_REG) {
                    if (!emit_load_gpr_sized_to_x20(buf, &instr->src2))
                        return HB_ERR_INTERNAL;
                } else {
                    emit_mov_imm_compact(buf, 20, (uint64_t)instr->src2.imm);
                }
                if (!emit_direct_mem_store_from_x20_tso(buf, &instr->src1))
                    return HB_ERR_INTERNAL;
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_store_operand_lazy);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_PUSH: {
            if (emit_native_push(buf, instr))
                return HB_OK;
            if (instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM)
                return emit_interp_ir_helper(buf, instr);
            emit_load_operand(buf, &instr->src1);
            emit_mov_reg(buf, 1, 20); /* X1 = value */
            emit_mov_reg(buf, 0, 19); /* X0 = ctx */
            emit_call_helper(buf, (void*)hb_jit_helper_push);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_POP: {
            if (emit_native_pop(buf, instr))
                return HB_OK;
            if (instr->dst.type != HB_OP_REG)
                return emit_interp_ir_helper(buf, instr);
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_pop);
            emit_return_if_helper_failed(buf);
            emit_mov_reg(buf, 20, 0); /* result in X0 */
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_CALL: {
            if (instr->src1.type != HB_OP_NONE) {
                if (emit_native_indirect_call(buf, instr))
                    return HB_OK;
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_call_operand);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            if (emit_native_direct_call(buf, instr))
                return HB_OK;
            /* Push first so stack faults do not commit the branch target. */
            emit_mov_imm64(buf, 1, instr->guest_addr + instr->guest_len); /* ret addr */
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_push);
            emit_return_if_helper_failed(buf);
            emit_set_pc_imm64(buf, instr->target);
            return HB_OK;
        }

        case HB_IR_RET: {
            if (emit_native_ret(buf, instr))
                return HB_OK;
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_pop);
            emit_return_if_helper_failed(buf);
            emit_str_x(buf, 0, 19, (uint32_t)offsetof(hb_context_t, pc));
            if (instr->src1.type == HB_OP_IMM && instr->src1.imm) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->src1.imm);
                emit_call_helper(buf, (void*)hb_jit_helper_adjust_stack);
                emit_return_if_helper_failed(buf);
            }
            return HB_OK;
        }

        case HB_IR_JMP: {
            if (instr->src1.type != HB_OP_NONE) {
                if (emit_native_indirect_jmp(buf, instr))
                    return HB_OK;
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_jmp_operand);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            emit_set_pc_imm64(buf, instr->target);
            return HB_OK;
        }

        case HB_IR_Jcc: {
            /* Layout:
             *   helper eval + cmp x0, #0
             *   b.ne target     (4 bytes)
             *   mov_imm64 fallthrough (16 bytes)
             *   str_x fallthrough (4 bytes)
             *   b skip_target   (4 bytes)
             *   mov_imm64 target (16 bytes)
             *   str_x target    (4 bytes)
             * Total fallthrough+skip = 24 bytes from b.cond start.
             * Target path starts at offset 28 from b.cond.
             * b at offset 24 must skip 24 bytes to land after target path.
             */
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_call_helper(buf, (void*)hb_jit_helper_eval_cond_lazy);
            emit_return_if_helper_failed(buf);
            emit_cmp_imm(buf, 0, 0);
            emit_bcond(buf, 1, 28);      /* taken -> target mov_imm64 */
            emit_mov_imm64(buf, 21, instr->guest_addr + instr->guest_len);
            emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
            emit_b(buf, 24);             /* skip target path (20 bytes) + 4 padding */
            emit_mov_imm64(buf, 20, instr->target);
            emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
            return HB_OK;
        }

        case HB_IR_LOOP:
        case HB_IR_JRCXZ: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_loop_branch);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_SETcc: {
            if (instr->dst.type != HB_OP_REG || instr->dst.reg_offset != 0) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
                emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_setcc_operand_lazy);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_setcc_lazy);
            return HB_OK;
        }

        case HB_IR_CMOVcc: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            if (instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
                emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_cmovcc_operand_lazy);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_mov_imm64(buf, 3, instr->src1.type == HB_OP_REG ? 1ULL : 0ULL);
            emit_mov_imm64(buf, 4, instr->src1.type == HB_OP_REG ? (uint64_t)instr->src1.reg : (uint64_t)instr->src1.imm);
            emit_mov_imm64(buf, 5, (uint64_t)instr->dst.size);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_cmovcc_lazy);
            return HB_OK;
        }

        case HB_IR_BSF:
        case HB_IR_TZCNT:
        case HB_IR_LZCNT:
        case HB_IR_BSR: {
            if (emit_native_bit_scan(buf, instr))
                return HB_OK;
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_bit_scan);
            return HB_OK;
        }

        case HB_IR_BSWAP: {
            if (emit_native_bswap(buf, instr))
                return HB_OK;
            return emit_interp_ir_helper(buf, instr);
        }

        case HB_IR_BT:
        case HB_IR_BTS:
        case HB_IR_BTR:
        case HB_IR_BTC: {
            if (emit_native_bit_test(buf, instr))
                return HB_OK;
            return emit_interp_ir_helper(buf, instr);
        }

        case HB_IR_CMPXCHG:
        case HB_IR_CMPXCHG8B:
        case HB_IR_XCHG:
        case HB_IR_XADD:
            return emit_atomic_ir_helper(buf, instr);

        case HB_IR_MOV_SEG:
        case HB_IR_CLD:
        case HB_IR_STD:
        case HB_IR_PUSHF:
        case HB_IR_POPF:
        case HB_IR_MOVS:
        case HB_IR_CMPS:
        case HB_IR_LODS:
        case HB_IR_SCAS:
        case HB_IR_STOS:
        case HB_IR_TRUNC:
        case HB_IR_XMM_QWORD_LANE_MOV:
        case HB_IR_PCMPEQB:
        case HB_IR_PCMPEQW:
        case HB_IR_PCMPEQD:
        case HB_IR_PCMPGTB:
        case HB_IR_PCMPGTW:
        case HB_IR_PCMPGTD:
        case HB_IR_PMOVMSKB:
        case HB_IR_MOVMSK:
        case HB_IR_PUNPCK:
        case HB_IR_PACKSSWB:
        case HB_IR_PACKUSWB:
        case HB_IR_PACKSSDW:
        case HB_IR_PMULLW:
        case HB_IR_PMULHW:
        case HB_IR_PMULHUW:
        case HB_IR_PMADDWD:
        case HB_IR_PADDSB:
        case HB_IR_PADDSW:
        case HB_IR_PADDUSB:
        case HB_IR_PADDUSW:
        case HB_IR_PAVGB:
        case HB_IR_PAVGW:
        case HB_IR_PSHUFB:
        case HB_IR_PINSRW:
        case HB_IR_PEXTRW:
        case HB_IR_PSHUF:
        case HB_IR_FSHUF:
        case HB_IR_PSRL:
        case HB_IR_PSRA:
        case HB_IR_PSLL:
        case HB_IR_PSRLQ:
        case HB_IR_PSLLQ:
        case HB_IR_PSRLDQ:
        case HB_IR_PSLLDQ:
        case HB_IR_MOVD:
        case HB_IR_CVTDQ2PD:
        case HB_IR_CVTDQ2PS:
        case HB_IR_CVTPS2DQ:
        case HB_IR_CVTTPS2DQ:
        case HB_IR_CVTPS2PD:
        case HB_IR_CVTPD2PS:
        /* MacRunner: CVTPD2DQ/CVTTPD2DQ (packed double -> packed dword, round/truncate)
         * were missing from the JIT-supported set, so any block containing them fell
         * back to the interpreter WHOLE-BLOCK. HK's hot double-math fns (exp2/pow:
         * UnityPlayer rva 0x19cc7be x7058, 0x19cc232/0x19cd6c6 x458) hit this in a tight
         * loop -> catastrophic slowdown/stall during render setup. They route through
         * emit_interp_ir_helper (per-instr interp call) like the other CVT ops; only
         * these 2 ops call the helper, the rest of the block stays native. */
        case HB_IR_CVTPD2DQ:
        case HB_IR_CVTTPD2DQ:
        case HB_IR_CVTSS2SD:
        case HB_IR_CVTSD2SS:
        case HB_IR_CVTSI2SD:
        case HB_IR_CVTSI2SS:
        case HB_IR_FSQRT:
        case HB_IR_FRSQRT:
        case HB_IR_FRCP:
        case HB_IR_FADD:
        case HB_IR_FSUB:
        case HB_IR_FMUL:
        case HB_IR_FDIV:
        case HB_IR_ADDSD:
        case HB_IR_SUBSD:
        case HB_IR_DIVSD:
        case HB_IR_MULSD:
        case HB_IR_DIVSS:
        case HB_IR_MULSS:
        case HB_IR_FMIN:
        case HB_IR_FMAX:
        case HB_IR_COMISS:
        case HB_IR_COMISD:
        case HB_IR_CVTTSD2SI:
        case HB_IR_CVTTSS2SI:
        /* MacRunner: CVTSS2SI/CVTSD2SI (NON-truncating scalar float->int, MXCSR
         * round-mode dependent) were missing from codegen (only the truncating
         * CVTTSS2SI/CVTTSD2SI were here). HK's float->int rounding helper (UnityPlayer
         * rva 0x19cc97c, the STMXCSR/cvtss2si/LDMXCSR sequence) whole-block-fell-back
         * on these -> the new hot loop after the CVTPD2DQ fix. Route via interp helper. */
        case HB_IR_CVTSS2SI:
        case HB_IR_CVTSD2SI:
        case HB_IR_PADD:
        case HB_IR_PSUB:
        case HB_IR_X87_FLD:
        case HB_IR_X87_FST:
        case HB_IR_X87_FSTP:
        case HB_IR_X87_FILD:
        case HB_IR_X87_FISTP:
        case HB_IR_X87_FLDCW:
        case HB_IR_X87_FNSTCW:
        case HB_IR_X87_FNSTSW:
        case HB_IR_X87_FLDENV:
        case HB_IR_X87_FNSTENV:
        case HB_IR_X87_FRSTOR:
        case HB_IR_X87_FNSAVE:
        case HB_IR_X87_FXSAVE:
        case HB_IR_X87_FXRSTOR:
        case HB_IR_X87_FADD:
        case HB_IR_X87_FMUL:
        case HB_IR_X87_FCOM:
        case HB_IR_X87_FCOMP:
        case HB_IR_X87_FSUB:
        case HB_IR_X87_FSUBR:
        case HB_IR_X87_FDIV:
        case HB_IR_X87_FDIVR:
        case HB_IR_X87_FADDP:
        case HB_IR_X87_FMULP:
        case HB_IR_X87_FCOMPP:
        case HB_IR_X87_FSUBP:
        case HB_IR_X87_FSUBRP:
        case HB_IR_X87_FDIVP:
        case HB_IR_X87_FDIVRP:
        case HB_IR_X87_FXCH:
        case HB_IR_X87_FRNDINT:
        case HB_IR_X87_FNCLEX:
        case HB_IR_X87_FNINIT:
        case HB_IR_HOST_CALL:
        case HB_IR_PUSHA:
        case HB_IR_POPA:
        case HB_IR_LDS:
        case HB_IR_LES:
        case HB_IR_LFS:
        case HB_IR_LGS:
            return emit_interp_ir_helper(buf, instr);

        case HB_IR_XMM_AND:
        case HB_IR_XMM_ANDN:
        case HB_IR_XMM_OR:
        case HB_IR_XORPS: {
            if (emit_native_xmm_logic(buf, instr))
                return HB_OK;
            return emit_interp_ir_helper(buf, instr);
        }

        case HB_IR_CWD: {
            if (emit_native_cwd(buf, instr))
                return HB_OK;
            return emit_interp_ir_helper(buf, instr);
        }

        case HB_IR_UNSUPPORTED:
        case HB_IR_FAULT:
            /* Genuine LIFT gaps (the lifter couldn't decode the x86 op): the
             * interpreter can't help either, so fail the block. */
            return HB_ERR_UNSUPPORTED_OPCODE;

        default:
            /* MacRunner 2026-06-19 (fallback-loop fix): a real IR op that the
             * lifter produced and the interpreter implements (all 163 IR ops),
             * but ARM64 codegen has no emitter for yet (e.g. RCR/RCL, the SSE
             * compare family). Previously this returned UNSUPPORTED_OPCODE -> the
             * whole block failed -> block-level interp fallback -> and when the
             * op was first in the block (steps=0) PC never advanced -> infinite
             * retry of the same op (the HK boot livelock). Route it to the
             * per-op interpreter helper instead: the op is interpreted inline and
             * the JIT block continues + advances PC normally. (Control-flow ops
             * are explicitly cased above, so default only reaches data ops, which
             * the per-op helper executes safely.) Closing the codegen emitters for
             * the hot ops is a separate SPEED follow-up, not a correctness gate. */
            return emit_interp_ir_helper(buf, instr);
    }
}

/* --- Helper implementations --- */
static void hb_jit_split_lock_acquire(void);
static void hb_jit_split_lock_release(void);

static bool hb_jit_helper_host_ptr_aligned(const void* ptr, size_t size) {
    return ptr && size && (((uintptr_t)ptr) & (size - 1u)) == 0;
}

static uint8_t hb_jit_helper_host_load_u8_acquire(const void* ptr) {
    return __atomic_load_n((const uint8_t*)ptr, __ATOMIC_ACQUIRE);
}

static uint16_t hb_jit_helper_host_load_u16_acquire(const void* ptr) {
    uint16_t value = 0;
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value)))
        return __atomic_load_n((const uint16_t*)ptr, __ATOMIC_ACQUIRE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(&value, ptr, sizeof(value));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return value;
}

static uint32_t hb_jit_helper_host_load_u32_acquire(const void* ptr) {
    uint32_t value = 0;
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value)))
        return __atomic_load_n((const uint32_t*)ptr, __ATOMIC_ACQUIRE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(&value, ptr, sizeof(value));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return value;
}

static uint64_t hb_jit_helper_host_load_u64_acquire(const void* ptr) {
    uint64_t value = 0;
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value)))
        return __atomic_load_n((const uint64_t*)ptr, __ATOMIC_ACQUIRE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(&value, ptr, sizeof(value));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return value;
}

static void hb_jit_helper_host_load_bytes_acquire(void* dst, const void* src, size_t size) {
    switch (size) {
        case 1: *(uint8_t*)dst = hb_jit_helper_host_load_u8_acquire(src); return;
        case 2: *(uint16_t*)dst = hb_jit_helper_host_load_u16_acquire(src); return;
        case 4: *(uint32_t*)dst = hb_jit_helper_host_load_u32_acquire(src); return;
        case 8: *(uint64_t*)dst = hb_jit_helper_host_load_u64_acquire(src); return;
        default:
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            memcpy(dst, src, size);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return;
    }
}

static hb_result_t hb_jit_helper_read_u8_tso(hb_context_t* ctx, uint64_t addr, uint8_t* out) {
    void* ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory || !out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(*out), HB_PERM_READ);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(*out), HB_PERM_READ);
#endif
    if (ptr) {
        *out = hb_jit_helper_host_load_u8_acquire(ptr);
        return HB_OK;
    }
    r = hb_memory_read_u8(ctx->memory, addr, out);
    if (r == HB_OK) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return r;
}

static hb_result_t hb_jit_helper_read_u16_tso(hb_context_t* ctx, uint64_t addr, uint16_t* out) {
    void* ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory || !out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(*out), HB_PERM_READ);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(*out), HB_PERM_READ);
#endif
    if (ptr) {
        *out = hb_jit_helper_host_load_u16_acquire(ptr);
        return HB_OK;
    }
    r = hb_memory_read_u16(ctx->memory, addr, out);
    if (r == HB_OK) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return r;
}

static hb_result_t hb_jit_helper_read_u32_tso(hb_context_t* ctx, uint64_t addr, uint32_t* out) {
    void* ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory || !out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(*out), HB_PERM_READ);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(*out), HB_PERM_READ);
#endif
    if (ptr) {
        *out = hb_jit_helper_host_load_u32_acquire(ptr);
        return HB_OK;
    }
    r = hb_memory_read_u32(ctx->memory, addr, out);
    if (r == HB_OK) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return r;
}

static hb_result_t hb_jit_helper_read_u64_tso(hb_context_t* ctx, uint64_t addr, uint64_t* out) {
    void* ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory || !out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(*out), HB_PERM_READ);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(*out), HB_PERM_READ);
#endif
    if (ptr) {
        *out = hb_jit_helper_host_load_u64_acquire(ptr);
        return HB_OK;
    }
    r = hb_memory_read_u64(ctx->memory, addr, out);
    if (r == HB_OK) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return r;
}

static hb_result_t hb_jit_helper_read_bytes_tso(hb_context_t* ctx, uint64_t addr,
                                                void* out, size_t size) {
    void* ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory || !out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, size, HB_PERM_READ);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, size, HB_PERM_READ);
#endif
    if (ptr) {
        hb_jit_helper_host_load_bytes_acquire(out, ptr, size);
        return HB_OK;
    }
    r = hb_memory_read(ctx->memory, (hb_gva_t)addr, out, size);
    if (r == HB_OK) __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return r;
}

static hb_result_t hb_jit_helper_write_u8_tso(hb_context_t* ctx, uint64_t addr, uint8_t value) {
    void* ptr;
    if (!ctx || !ctx->memory) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(value), HB_PERM_WRITE);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(value), HB_PERM_WRITE);
#endif
    if (ptr) {
        __atomic_store_n((uint8_t*)ptr, value, __ATOMIC_RELEASE);
        return HB_OK;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return hb_memory_write_u8(ctx->memory, addr, value);
}

static hb_result_t hb_jit_helper_write_u16_tso(hb_context_t* ctx, uint64_t addr, uint16_t value) {
    void* ptr;
    if (!ctx || !ctx->memory) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(value), HB_PERM_WRITE);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(value), HB_PERM_WRITE);
#endif
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value))) {
        __atomic_store_n((uint16_t*)ptr, value, __ATOMIC_RELEASE);
        return HB_OK;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return hb_memory_write_u16(ctx->memory, addr, value);
}

static hb_result_t hb_jit_helper_write_u32_tso(hb_context_t* ctx, uint64_t addr, uint32_t value) {
    void* ptr;
    if (!ctx || !ctx->memory) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(value), HB_PERM_WRITE);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(value), HB_PERM_WRITE);
#endif
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value))) {
        __atomic_store_n((uint32_t*)ptr, value, __ATOMIC_RELEASE);
        return HB_OK;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return hb_memory_write_u32(ctx->memory, addr, value);
}

static hb_result_t hb_jit_helper_write_u64_tso(hb_context_t* ctx, uint64_t addr, uint64_t value) {
    void* ptr;
    void* mem_ptr;
    hb_result_t r;
    if (!ctx || !ctx->memory) return HB_ERR_MEMORY_FAULT;
    mem_ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(value), HB_PERM_WRITE);
    ptr = mem_ptr;
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(value), HB_PERM_WRITE);
#endif
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value))) {
        __atomic_store_n((uint64_t*)ptr, value, __ATOMIC_RELEASE);
        return HB_OK;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r = hb_memory_write_u64(ctx->memory, addr, value);
    if (r != HB_OK) {
        static int traced;
        if (traced++ < 8)
            fprintf(stderr, "macrunner-hb-tso-write-fail: addr=0x%llx r=%d mem_ptr=%p live_ptr=%p\n",
                    (unsigned long long)addr, (int)r, mem_ptr, ptr);
    }
    return r;
}

static hb_result_t hb_jit_helper_write_bytes_tso(hb_context_t* ctx, uint64_t addr,
                                                 const void* src, size_t size) {
    void* ptr;
    if (!ctx || !ctx->memory || !src) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, size, HB_PERM_WRITE);
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, size, HB_PERM_WRITE);
#endif
    if (ptr) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memcpy(ptr, src, size);
        return HB_OK;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return hb_memory_write(ctx->memory, (hb_gva_t)addr, src, size);
}

static hb_result_t hb_jit_helper_exchange_u64_tso(hb_context_t* ctx, uint64_t addr,
                                                  uint64_t value, uint64_t* old_out) {
    void* ptr;
    hb_result_t r;
    uint64_t old = 0;

    if (!ctx || !ctx->memory || !old_out) return HB_ERR_MEMORY_FAULT;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, sizeof(value),
                             (hb_perm_t)(HB_PERM_READ | HB_PERM_WRITE));
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, sizeof(value),
                                         (hb_perm_t)(HB_PERM_READ | HB_PERM_WRITE));
#endif
    if (hb_jit_helper_host_ptr_aligned(ptr, sizeof(value))) {
        *old_out = __atomic_exchange_n((uint64_t*)ptr, value, __ATOMIC_SEQ_CST);
        return HB_OK;
    }

    hb_jit_split_lock_acquire();
    r = hb_jit_helper_read_u64_tso(ctx, addr, &old);
    if (r == HB_OK) r = hb_jit_helper_write_u64_tso(ctx, addr, value);
    hb_jit_split_lock_release();
    if (r == HB_OK) *old_out = old;
    return r;
}

static bool hb_jit_operand_is_guest_mem(const hb_ir_operand_t* op) {
    return op && op->type == HB_OP_MEM;
}

static void hb_jit_helper_acquire_after_operand_read(const hb_ir_operand_t* op) {
    if (hb_jit_operand_is_guest_mem(op))
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static void hb_jit_helper_release_before_operand_write(const hb_ir_operand_t* op) {
    if (hb_jit_operand_is_guest_mem(op))
        __atomic_thread_fence(__ATOMIC_RELEASE);
}

uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr) {
    uint64_t val = 0;
    if (!ctx) return 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    hb_result_t r = hb_jit_helper_read_u64_tso(ctx, addr, &val);
    ctx->last_result = r;
    return val;
}

static uint64_t hb_jit_helper_load_sized_value(hb_context_t* ctx, uint64_t addr, hb_size_t size) {
    uint64_t val = 0;
    hb_result_t r = HB_OK;
    if (!ctx) return 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    switch (size) {
        case HB_SIZE_8: {
            uint8_t v = 0;
            r = hb_jit_helper_read_u8_tso(ctx, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_16: {
            uint16_t v = 0;
            r = hb_jit_helper_read_u16_tso(ctx, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_32: {
            uint32_t v = 0;
            r = hb_jit_helper_read_u32_tso(ctx, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_64:
        default:
            r = hb_jit_helper_read_u64_tso(ctx, addr, &val);
            break;
    }
    ctx->last_result = r;
    return val;
}

void hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                     uint64_t dst_reg, uint64_t dst_size,
                                     uint64_t dst_reg_offset) {
    hb_ir_operand_t dst;
    hb_size_t size = dst_size ? (hb_size_t)dst_size : HB_SIZE_64;
    if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-load_to_reg_sized: codegen-addr=0x%llx size=%u dst_reg=%llu\n", (unsigned long long)addr, (unsigned)size, (unsigned long long)dst_reg);
    uint64_t val = hb_jit_helper_load_sized_value(ctx, addr, size);
    if (!ctx || ctx->last_result != HB_OK) return;

    memset(&dst, 0, sizeof(dst));
    dst.type = HB_OP_REG;
    dst.reg = (uint8_t)dst_reg;
    dst.size = size;
    dst.reg_offset = (uint8_t)dst_reg_offset;
    ctx->last_result = hb_flags_write_operand_value(ctx, &dst, val);
}

void hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &val);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&instr->src1);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &instr->dst, val);
    ctx->last_result = r;
}

void hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &val);
    if (r == HB_OK) hb_jit_helper_release_before_operand_write(&instr->src1);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &instr->src1, val);
    ctx->last_result = r;
}

void hb_jit_helper_store_u64(hb_context_t* ctx, uint64_t addr, uint64_t val) {
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    ctx->last_result = hb_jit_helper_write_u64_tso(ctx, addr, val);
}

void hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr, uint64_t val, uint64_t size) {
    if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-store_sized: addr=0x%llx size=%llu val=0x%llx\n", (unsigned long long)addr, (unsigned long long)size, (unsigned long long)val);
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    switch ((hb_size_t)size) {
        case HB_SIZE_8:
            ctx->last_result = hb_jit_helper_write_u8_tso(ctx, addr, (uint8_t)val);
            break;
        case HB_SIZE_16:
            ctx->last_result = hb_jit_helper_write_u16_tso(ctx, addr, (uint16_t)val);
            break;
        case HB_SIZE_32:
            ctx->last_result = hb_jit_helper_write_u32_tso(ctx, addr, (uint32_t)val);
            break;
        case HB_SIZE_64:
        default:
            ctx->last_result = hb_jit_helper_write_u64_tso(ctx, addr, val);
            break;
    }
}

uint64_t hb_jit_helper_pop(hb_context_t* ctx) {
    if (!ctx) return 0;
    uint64_t val = 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    hb_result_t r;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t val32 = 0;
        r = hb_jit_helper_read_u32_tso(ctx, ctx->regs.x86.esp, &val32);
        val = val32;
    } else {
        r = hb_jit_helper_read_u64_tso(ctx, ctx->regs.x64.rsp, &val);
    }
    ctx->last_result = r;
    if (r != HB_OK) return 0;
    if (ctx->mode == HB_MODE_32BIT)
        ctx->regs.x86.esp += 4;
    else
        ctx->regs.x64.rsp += 8;
    return val;
}

void hb_jit_helper_push(hb_context_t* ctx, uint64_t val) {
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    hb_result_t r;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t new_esp = ctx->regs.x86.esp - 4;
        r = hb_jit_helper_write_u32_tso(ctx, new_esp, (uint32_t)val);
        if (r == HB_OK) ctx->regs.x86.esp = new_esp;
    } else {
        uint64_t new_rsp = ctx->regs.x64.rsp - 8;
        r = hb_jit_helper_write_u64_tso(ctx, new_rsp, val);
        if (r == HB_OK) ctx->regs.x64.rsp = new_rsp;
    }
    if (r != HB_OK) {
        static int traced;
        if (traced++ < 8)
            fprintf(stderr, "macrunner-hb-push-fail: rsp=0x%llx r=%d mode=%d\n",
                    (unsigned long long)(ctx->mode == HB_MODE_32BIT ?
                        (uint64_t)ctx->regs.x86.esp : ctx->regs.x64.rsp), (int)r, (int)ctx->mode);
    }
    ctx->last_result = r;
}

static void trace_x86_branch_operand(hb_context_t* ctx, const hb_ir_instr_t* instr,
                                     const char* kind, uint64_t target,
                                     hb_result_t result);

/* MacRunner diag: guest called/jumped a NULL function pointer. Log the call-site
 * guest RIP + the target SOURCE (which [mem]/reg loaded 0) with CLEAN guest state,
 * before the synthesized exec fault. Unix-side fprintf works (HB in ntdll.so). */
static void hb_diag_nullcall(hb_context_t* ctx, const hb_ir_instr_t* instr, const char* where) {
    static int nc_n;
    uint64_t ea = 0, base = 0;
    if (!ctx || !instr || nc_n++ >= 24) return;
    if (instr->src1.type == HB_OP_MEM) {
        base = (instr->src1.mem.base < HB_REG_COUNT) ?
               hb_context_read_reg_value(ctx, instr->src1.mem.base) : 0;
        uint64_t idx = (instr->src1.mem.index < HB_REG_COUNT) ?
               hb_context_read_reg_value(ctx, instr->src1.mem.index) : 0;
        ea = base + idx * instr->src1.mem.scale + (uint64_t)instr->src1.mem.disp;
    }
    fprintf(stderr, "macrunner-hb-jit-nullcall: where=%s guest_pc=0x%llx len=%u src_type=%d "
            "src_reg=%d mem_base=%d mem_idx=%d mem_scale=%d mem_disp=0x%llx ea=0x%llx base_val=0x%llx "
            "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx\n",
            where, (unsigned long long)instr->guest_addr, instr->guest_len, (int)instr->src1.type,
            (instr->src1.type == HB_OP_REG) ? (int)instr->src1.reg : -1,
            (instr->src1.type == HB_OP_MEM) ? (int)instr->src1.mem.base : -1,
            (instr->src1.type == HB_OP_MEM) ? (int)instr->src1.mem.index : -1,
            (instr->src1.type == HB_OP_MEM) ? (int)instr->src1.mem.scale : 0,
            (unsigned long long)((instr->src1.type == HB_OP_MEM) ? (uint64_t)instr->src1.mem.disp : 0),
            (unsigned long long)ea, (unsigned long long)base,
            (unsigned long long)ctx->regs.x64.rax, (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->regs.x64.rdx, (unsigned long long)ctx->regs.x64.rbx,
            (unsigned long long)ctx->regs.x64.rsp, (unsigned long long)ctx->regs.x64.rbp,
            (unsigned long long)ctx->regs.x64.rsi, (unsigned long long)ctx->regs.x64.rdi);
    fflush(stderr);
}

void hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t target = instr ? instr->target : 0;
    uint64_t ret_addr;
    hb_result_t r;
    if (!ctx || !instr) return;
    if (instr->src1.type != HB_OP_NONE) {
        r = hb_flags_read_operand_value(ctx, &instr->src1, &target);
        if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&instr->src1);
        trace_x86_branch_operand(ctx, instr, "call", target, r);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
    }
    if (!target) {
        hb_diag_nullcall(ctx, instr, "call");
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return;
    }
    /* MacRunner: trace import-region (0x6f00...) call targets so we can compare the
     * CreateDXGIFactory2 call (works) vs the D3D11CreateDevice call (device lost).
     * Gated by env; the run-loop dispatches pc in the import region via the import
     * thunk -> pe_call12. If D3D11CreateDevice's call does not appear here, it takes
     * a different codegen path. */
    if (target >= 0x00006f0000001b00ULL && target < 0x00006f0000001c00ULL) {
        static int co_n;
        if (getenv("MACRUNNER_TRACE_CALLOP") && co_n++ < 200)
            fprintf(stderr, "macrunner-hb-callop-import: call_site_guest_pc=0x%llx target=0x%llx "
                    "rcx=0x%llx rdx=0x%llx r8=0x%llx rsp=0x%llx\n",
                    (unsigned long long)instr->guest_addr, (unsigned long long)target,
                    (unsigned long long)ctx->regs.x64.rcx, (unsigned long long)ctx->regs.x64.rdx,
                    (unsigned long long)ctx->regs.x64.r8, (unsigned long long)ctx->regs.x64.rsp);
    }
    ret_addr = instr->guest_addr + instr->guest_len;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t new_esp = ctx->regs.x86.esp - 4;
        r = hb_jit_helper_write_u32_tso(ctx, new_esp, (uint32_t)ret_addr);
        if (r == HB_OK) ctx->regs.x86.esp = new_esp;
    } else {
        uint64_t new_rsp = ctx->regs.x64.rsp - 8;
        r = hb_jit_helper_write_u64_tso(ctx, new_rsp, ret_addr);
        if (r == HB_OK) ctx->regs.x64.rsp = new_rsp;
    }
    if (r != HB_OK) {
        static int traced;
        if (traced++ < 8)
            fprintf(stderr, "macrunner-hb-callop-fail: rsp=0x%llx r=%d target=0x%llx\n",
                    (unsigned long long)(ctx->mode == HB_MODE_32BIT ?
                        (uint64_t)ctx->regs.x86.esp : ctx->regs.x64.rsp), (int)r,
                    (unsigned long long)target);
        ctx->last_result = r;
        return;
    }
    ctx->pc = target;
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rip = target;
    else ctx->regs.x86.eip = (uint32_t)target;
    ctx->last_result = HB_OK;
}

static bool hb_jit_read_guest_u8(hb_context_t* ctx, uint64_t addr, uint8_t* out) {
    return ctx && ctx->memory && out &&
           hb_jit_helper_read_u8_tso(ctx, addr, out) == HB_OK;
}

static bool hb_jit_read_guest_u32(hb_context_t* ctx, uint64_t addr, uint32_t* out) {
    return ctx && ctx->memory && out &&
           hb_jit_helper_read_u32_tso(ctx, addr, out) == HB_OK;
}

static bool hb_jit_read_guest_u64(hb_context_t* ctx, uint64_t addr, uint64_t* out) {
    return ctx && ctx->memory && out &&
           hb_jit_helper_read_u64_tso(ctx, addr, out) == HB_OK;
}

static bool hb_jit_xfg_dispatch_resolves_to_rax(hb_context_t* ctx, uint64_t target) {
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    uint32_t disp32 = 0;
    uint64_t slot = 0;
    uint64_t next = 0;

    if (!hb_jit_read_guest_u8(ctx, target, &b0) ||
        !hb_jit_read_guest_u8(ctx, target + 1, &b1))
        return false;
    if (b0 == 0xff && b1 == 0xe0)
        return true;
    if (b0 != 0xff || b1 != 0x25)
        return false;
    if (!hb_jit_read_guest_u32(ctx, target + 2, &disp32))
        return false;
    slot = target + 6 + (int64_t)(int32_t)disp32;
    if (!hb_jit_read_guest_u64(ctx, slot, &next))
        return false;
    return hb_jit_read_guest_u8(ctx, next, &b0) &&
           hb_jit_read_guest_u8(ctx, next + 1, &b1) &&
           b0 == 0xff && b1 == 0xe0;
}

static hb_result_t hb_jit_read_guest_u8_result(hb_context_t* ctx, uint64_t addr,
                                               uint8_t* out) {
    return hb_jit_helper_read_u8_tso(ctx, addr, out);
}

static hb_result_t hb_jit_read_guest_u64_result(hb_context_t* ctx, uint64_t addr,
                                                uint64_t* out) {
    return hb_jit_helper_read_u64_tso(ctx, addr, out);
}

static hb_result_t hb_jit_call_target64(hb_context_t* ctx, uint64_t target,
                                        uint64_t ret_addr) {
    uint64_t new_rsp = ctx->regs.x64.rsp - 8;
    hb_result_t r = hb_jit_helper_write_u64_tso(ctx, new_rsp, ret_addr);
    if (r != HB_OK) return r;
    ctx->regs.x64.rsp = new_rsp;
    ctx->pc = target;
    ctx->regs.x64.rip = target;
    return HB_OK;
}

static int trace_x86_branch_operand_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_X86_LOW_PC");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_x86_branch_mem_addr(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t base = 0;
    uint64_t index = 0;
    uint64_t ea;
    if (!ctx || !op || op->type != HB_OP_MEM) return 0;
    if (op->mem.base < HB_REG_COUNT) base = hb_context_read_reg_value(ctx, op->mem.base);
    if (op->mem.index < HB_REG_COUNT) index = hb_context_read_reg_value(ctx, op->mem.index);
    if (op->mem.segment == 0x64) base += ctx->fs_base;
    else if (op->mem.segment == 0x65) base += ctx->gs_base;
    ea = base + index * op->mem.scale + (uint64_t)op->mem.disp;
    if (ctx->mode == HB_MODE_32BIT || op->mem.addr32) ea = (uint32_t)ea;
    return ea;
}

static void trace_x86_branch_operand(hb_context_t* ctx, const hb_ir_instr_t* instr,
                                     const char* kind, uint64_t target,
                                     hb_result_t result) {
    uint64_t mem_addr = 0;
    uint32_t mem_value = 0;
    hb_result_t mem_read = HB_ERR_INVALID_ARG;

    if (!trace_x86_branch_operand_enabled() || !ctx || !instr ||
        ctx->mode != HB_MODE_32BIT)
        return;
    if (target >= 0x10000u && instr->guest_addr != 0x7bdc6da0u &&
        instr->guest_addr != 0x7bdc5e9au)
        return;
    if (instr->src1.type == HB_OP_MEM) {
        mem_addr = trace_x86_branch_mem_addr(ctx, &instr->src1);
        mem_read = hb_jit_helper_read_u32_tso(ctx, mem_addr, &mem_value);
    }
    fprintf(stderr,
            "macrunner-hb-x86-branch-helper: kind=%s guest=%p op=%u result=%s "
            "target=%p eax=%08x edx=%08x esp=%08x src{t=%u sz=%u reg=%u imm=%lld "
            "mem=(base=%u index=%u scale=%u disp=%lld addr32=%u addr=%p read=%s value=%08x)}\n",
            kind, (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->op,
            hb_result_string(result), (void*)(uintptr_t)target,
            ctx->regs.x86.eax, ctx->regs.x86.edx, ctx->regs.x86.esp,
            (unsigned)instr->src1.type, (unsigned)instr->src1.size,
            (unsigned)instr->src1.reg, (long long)instr->src1.imm,
            (unsigned)instr->src1.mem.base, (unsigned)instr->src1.mem.index,
            (unsigned)instr->src1.mem.scale, (long long)instr->src1.mem.disp,
            (unsigned)instr->src1.mem.addr32, (void*)(uintptr_t)mem_addr,
            instr->src1.type == HB_OP_MEM ? hb_result_string(mem_read) : "n/a",
            mem_value);
    fflush(stderr);
}

void hb_jit_helper_exec_xfg_dispatch_call(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t target = instr ? instr->target : 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    if (ctx->mode != HB_MODE_64BIT || instr->src1.type == HB_OP_NONE) {
        hb_jit_helper_exec_call_operand(ctx, instr);
        return;
    }

    r = hb_flags_read_operand_value(ctx, &instr->src1, &target);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&instr->src1);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    if (!target) {
        hb_diag_nullcall(ctx, instr, "xfg");
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return;
    }

    uint64_t direct = ctx->regs.x64.rax;
    if (!direct || !hb_jit_xfg_dispatch_resolves_to_rax(ctx, target)) {
        hb_jit_helper_exec_call_operand(ctx, instr);
        return;
    }

    ctx->last_result = hb_jit_call_target64(ctx, direct, instr->guest_addr + instr->guest_len);
}

void hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t target = instr ? instr->target : 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    if (instr->src1.type != HB_OP_NONE) {
        r = hb_flags_read_operand_value(ctx, &instr->src1, &target);
        if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&instr->src1);
        trace_x86_branch_operand(ctx, instr, "jmp", target, r);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
    }
    if (!target) {
        hb_diag_nullcall(ctx, instr, "jmp");
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return;
    }
    ctx->pc = target;
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rip = target;
    else ctx->regs.x86.eip = (uint32_t)target;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT)
        ctx->regs.x86.esp += (uint32_t)delta;
    else
        ctx->regs.x64.rsp += delta;
    ctx->last_result = HB_OK;
}

uint64_t hb_jit_helper_call(hb_context_t* ctx, uint64_t target, uint64_t ret_addr) {
    (void)ctx; (void)target; (void)ret_addr;
    return 0;
}

uint64_t hb_jit_helper_exec_binop_lazy(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                       uint64_t src1_reg, uint64_t src2_is_reg,
                                       uint64_t src2_value, uint64_t size) {
    return hb_flags_exec_binop(ctx, (hb_ir_op_t)op, dst_reg, 0, src1_reg, 0,
                               src2_is_reg != 0, src2_value, 0, (hb_size_t)size);
}

void hb_jit_helper_exec_cmp_test_lazy(hb_context_t* ctx, uint64_t op,
                                      uint64_t src1_is_reg, uint64_t src1_value,
                                      uint64_t src2_is_reg, uint64_t src2_value,
                                      uint64_t size) {
    hb_flags_exec_cmp_test(ctx, (hb_ir_op_t)op, src1_is_reg != 0, src1_value, 0,
                           src2_is_reg != 0, src2_value, 0, (hb_size_t)size);
}

void hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t lhs = 0, rhs = 0;
    uint64_t result = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &lhs);
    if (r != HB_OK) { ctx->last_result = r; return; }
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    r = hb_flags_read_operand_value(ctx, &instr->src2, &rhs);
    if (r != HB_OK) { ctx->last_result = r; return; }
    hb_jit_helper_acquire_after_operand_read(&instr->src2);
    result = (instr->op == HB_IR_TEST) ? (lhs & rhs) : (lhs - rhs);
    hb_lazy_flags_note(ctx, instr->op == HB_IR_TEST ? HB_LAZY_FLAGS_TEST : HB_LAZY_FLAGS_CMP,
                       instr->src1.size, lhs, rhs, result, 0);
    ctx->last_result = HB_OK;
}

uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc) {
    bool value = false;
    hb_result_t r = hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value);
    if (ctx) ctx->last_result = r;
    if (r != HB_OK) return 0;
    return value ? 1 : 0;
}

static uint64_t hb_jit_trunc_to_size(uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return value & 0xffu;
        case HB_SIZE_16: return value & 0xffffu;
        case HB_SIZE_32: return value & 0xffffffffu;
        case HB_SIZE_64:
        default: return value;
    }
}

static int64_t hb_jit_sign_extend_from_size(uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  return (int8_t)value;
        case HB_SIZE_16: return (int16_t)value;
        case HB_SIZE_32: return (int32_t)value;
        case HB_SIZE_64:
        default:         return (int64_t)value;
    }
}

static size_t hb_jit_size_bytes(hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return 1;
        case HB_SIZE_16: return 2;
        case HB_SIZE_32: return 4;
        case HB_SIZE_64: return 8;
        default: return 0;
    }
}

#ifdef __APPLE__
static void* hb_jit_live_host_ptr(uint64_t addr, size_t bytes, hb_perm_t perms) {
    mach_vm_address_t region = (mach_vm_address_t)addr;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr;
    vm_prot_t needed = 0;

    if (!addr || !bytes || addr + bytes < addr) return NULL;
    if ((perms & HB_PERM_READ) != 0) needed |= VM_PROT_READ;
    if ((perms & HB_PERM_WRITE) != 0) needed |= VM_PROT_WRITE;
    if (!needed) needed = VM_PROT_READ;
    kr = mach_vm_region(mach_task_self(), &region, &region_size,
                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                        &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS) return NULL;
    if (region > addr || region + region_size < addr + bytes) return NULL;
    if ((info.protection & needed) != needed) return NULL;
    if ((perms & HB_PERM_WRITE) && (info.protection & VM_PROT_EXECUTE)) { if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-live_host_ptr: addr=0x%llx perms=%d EXEC-write-guard -> NULL\n", (unsigned long long)addr, (int)perms); return NULL; }
    if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-live_host_ptr: addr=0x%llx perms=%d mach_prot=0x%x -> IDENTITY host=%p\n", (unsigned long long)addr, (int)perms, (int)info.protection, (void*)(uintptr_t)addr);
    return (void*)(uintptr_t)addr;
}

static void hb_jit_trace_live_atomic_reject(const char* op, uint64_t pc, uint64_t instr_pc,
                                            uint64_t addr, size_t bytes) {
    mach_vm_address_t region = (mach_vm_address_t)addr;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr;

    memset(&info, 0, sizeof(info));
    kr = mach_vm_region(mach_task_self(), &region, &region_size,
                        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                        &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    fprintf(stderr,
            "macrunner-hb-atomic-live-reject: op=%s pc=0x%llx instr=0x%llx addr=0x%llx "
            "size=%zu mach_kr=%d region=0x%llx region_end=0x%llx prot=0x%x max=0x%x\n",
            op ? op : "?",
            (unsigned long long)pc,
            (unsigned long long)instr_pc,
            (unsigned long long)addr,
            bytes,
            kr,
            (unsigned long long)region,
            (unsigned long long)(region + region_size),
            info.protection,
            info.max_protection);
}
#endif

static uint64_t hb_jit_resolve_addr(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t base = 0;
    uint64_t index = 0;

    if (!ctx || !op || op->type != HB_OP_MEM) return 0;
    if (op->mem.base < HB_REG_COUNT) {
        base = op->mem.base == HB_REG_RIP ? ctx->pc : hb_context_read_reg_value(ctx, op->mem.base);
    }
    if (op->mem.index < HB_REG_COUNT)
        index = hb_context_read_reg_value(ctx, op->mem.index);
    if (op->mem.segment == 0x64) base += ctx->fs_base;
    else if (op->mem.segment == 0x65) base += ctx->gs_base;
    if (ctx->mode == HB_MODE_32BIT || op->mem.addr32) {
        uint32_t base32 = (uint32_t)base;
        uint32_t index32 = (uint32_t)index;
        return (uint32_t)(base32 + index32 * op->mem.scale + (uint32_t)op->mem.disp);
    }
    return base + index * op->mem.scale + (uint64_t)op->mem.disp;
}

static void* hb_jit_atomic_host_ptr(hb_context_t* ctx, const hb_ir_operand_t* op,
                                    hb_size_t size, uint64_t* addr_out) {
    size_t bytes = hb_jit_size_bytes(size);
    uint64_t addr;
    void* ptr;

    if (!ctx || !ctx->memory || !op || op->type != HB_OP_MEM || !bytes) return NULL;
    addr = hb_jit_resolve_addr(ctx, op);
    if ((addr & (bytes - 1)) != 0) return NULL;
    if (((addr & 63u) + bytes) > 64u) return NULL;
    ptr = hb_memory_host_ptr(ctx->memory, (hb_gva_t)addr, bytes,
                             (hb_perm_t)(HB_PERM_READ | HB_PERM_WRITE));
#ifdef __APPLE__
    if (!ptr) ptr = hb_jit_live_host_ptr(addr, bytes,
                                         (hb_perm_t)(HB_PERM_READ | HB_PERM_WRITE));
#endif
    if (!ptr || (((uintptr_t)ptr) & (bytes - 1)) != 0) return NULL;
    if (addr_out) *addr_out = addr;
    return ptr;
}

static volatile uint32_t hb_jit_split_lock_gate;
static volatile uint64_t hb_jit_atomic_helper_trace_count;
static volatile uint64_t hb_jit_block_atomic_trace_count;
static volatile uint64_t hb_jit_atomic_xadd_trace_count;

static bool hb_jit_trace_atomics_enabled(void) {
    static int cached = -1;
    const char* env;
    if (cached >= 0) return cached != 0;
    env = getenv("MACRUNNER_HB_TRACE_ATOMICS");
    cached = (env && env[0] && env[0] != '0') ? 1 : 0;
    return cached != 0;
}

static void hb_jit_split_lock_acquire(void) {
    while (__atomic_exchange_n(&hb_jit_split_lock_gate, 1u, __ATOMIC_ACQUIRE))
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void hb_jit_split_lock_release(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&hb_jit_split_lock_gate, 0u, __ATOMIC_RELEASE);
}

static hb_result_t hb_jit_atomic_read_mem_value(hb_context_t* ctx,
                                                const hb_ir_operand_t* op,
                                                hb_size_t size,
                                                uint64_t* addr_out,
                                                uint64_t* value_out) {
    size_t bytes = hb_jit_size_bytes(size);
    uint64_t addr;
    uint64_t value = 0;
    hb_result_t r;

    if (!ctx || !ctx->memory || !op || op->type != HB_OP_MEM ||
        !bytes || !value_out)
        return HB_ERR_UNSUPPORTED_FEATURE;
    addr = hb_jit_resolve_addr(ctx, op);
    r = hb_jit_helper_read_bytes_tso(ctx, addr, &value, bytes);
    if (r != HB_OK) return r;
    if (addr_out) *addr_out = addr;
    *value_out = hb_jit_trunc_to_size(value, size);
    return HB_OK;
}

static hb_result_t hb_jit_atomic_write_mem_value(hb_context_t* ctx,
                                                 uint64_t addr,
                                                 hb_size_t size,
                                                 uint64_t value) {
    size_t bytes = hb_jit_size_bytes(size);
    uint64_t tmp = hb_jit_trunc_to_size(value, size);

    if (!ctx || !ctx->memory || !bytes)
        return HB_ERR_UNSUPPORTED_FEATURE;
    return hb_jit_helper_write_bytes_tso(ctx, addr, &tmp, bytes);
}

static hb_result_t hb_jit_atomic_cmpxchg(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
    void* ptr;
    uint64_t addr = 0;
    uint64_t src_val = 0;
    uint64_t acc, old = 0;
    bool equal = false;
    hb_result_t r;

    if (!size) size = instr->src2.size ? instr->src2.size : HB_SIZE_32;
    ptr = hb_jit_atomic_host_ptr(ctx, &instr->src1, size, &addr);
    if (!ptr) {
        if (hb_jit_trace_atomics_enabled()) {
            addr = hb_jit_resolve_addr(ctx, &instr->src1);
#ifdef __APPLE__
            hb_jit_trace_live_atomic_reject("cmpxchg", ctx->pc, instr->guest_addr,
                                            addr, hb_jit_size_bytes(size));
#else
            fprintf(stderr,
                    "macrunner-hb-atomic-live-reject: op=cmpxchg pc=0x%llx instr=0x%llx addr=0x%llx size=%zu\n",
                    (unsigned long long)ctx->pc,
                    (unsigned long long)instr->guest_addr,
                    (unsigned long long)addr,
                    hb_jit_size_bytes(size));
#endif
        }
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    acc = hb_jit_trunc_to_size(hb_context_read_reg_value(ctx, HB_REG_RAX), size);
    src_val = hb_jit_trunc_to_size(src_val, size);

    switch (size) {
        case HB_SIZE_8: {
            uint8_t expected = (uint8_t)acc;
            equal = __atomic_compare_exchange_n((uint8_t*)ptr, &expected, (uint8_t)src_val,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expected;
            break;
        }
        case HB_SIZE_16: {
            uint16_t expected = (uint16_t)acc;
            equal = __atomic_compare_exchange_n((uint16_t*)ptr, &expected, (uint16_t)src_val,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expected;
            break;
        }
        case HB_SIZE_32: {
            uint32_t expected = (uint32_t)acc;
            equal = __atomic_compare_exchange_n((uint32_t*)ptr, &expected, (uint32_t)src_val,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expected;
            break;
        }
        case HB_SIZE_64:
        default: {
            uint64_t expected = acc;
            equal = __atomic_compare_exchange_n((uint64_t*)ptr, &expected, src_val,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            old = expected;
            break;
        }
    }

    old = hb_jit_trunc_to_size(old, size);
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, size, acc, old, acc - old, 0);
    if (!equal)
        hb_context_write_reg_value_sized(ctx, HB_REG_RAX, old, size);
    return HB_OK;
}

static hb_result_t hb_jit_atomic_cmpxchg_split_locked(hb_context_t* ctx,
                                                      const hb_ir_instr_t* instr) {
    hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
    uint64_t src_val = 0;
    uint64_t addr = 0;
    uint64_t acc;
    uint64_t old = 0;
    bool equal;
    hb_result_t r;

    if (!size) size = instr->src2.size ? instr->src2.size : HB_SIZE_32;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    r = hb_jit_atomic_read_mem_value(ctx, &instr->src1, size, &addr, &old);
    if (r != HB_OK) return r;

    acc = hb_jit_trunc_to_size(hb_context_read_reg_value(ctx, HB_REG_RAX), size);
    src_val = hb_jit_trunc_to_size(src_val, size);
    equal = old == acc;
    if (equal) {
        r = hb_jit_atomic_write_mem_value(ctx, addr, size, src_val);
        if (r != HB_OK) return r;
    } else {
        hb_context_write_reg_value_sized(ctx, HB_REG_RAX, old, size);
    }
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, size, acc, old, acc - old, 0);
    return HB_OK;
}

static hb_result_t hb_jit_atomic_xchg(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_size_t size = instr->src1.size ? instr->src1.size : instr->src2.size;
    void* ptr;
    uint64_t src_val = 0;
    uint64_t old = 0;
    hb_result_t r;

    if (!size) size = HB_SIZE_32;
    ptr = hb_jit_atomic_host_ptr(ctx, &instr->src1, size, NULL);
    if (!ptr) return HB_ERR_UNSUPPORTED_FEATURE;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    src_val = hb_jit_trunc_to_size(src_val, size);

    switch (size) {
        case HB_SIZE_8:  old = __atomic_exchange_n((uint8_t*)ptr, (uint8_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_16: old = __atomic_exchange_n((uint16_t*)ptr, (uint16_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_32: old = __atomic_exchange_n((uint32_t*)ptr, (uint32_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_64:
        default:         old = __atomic_exchange_n((uint64_t*)ptr, src_val, __ATOMIC_SEQ_CST); break;
    }
    return hb_flags_write_operand_value(ctx, &instr->src2, hb_jit_trunc_to_size(old, size));
}

static hb_result_t hb_jit_atomic_xchg_split_locked(hb_context_t* ctx,
                                                   const hb_ir_instr_t* instr) {
    hb_size_t size = instr->src1.size ? instr->src1.size : instr->src2.size;
    uint64_t src_val = 0;
    uint64_t addr = 0;
    uint64_t old = 0;
    hb_result_t r;

    if (!size) size = HB_SIZE_32;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    r = hb_jit_atomic_read_mem_value(ctx, &instr->src1, size, &addr, &old);
    if (r != HB_OK) return r;
    r = hb_jit_atomic_write_mem_value(ctx, addr, size, src_val);
    if (r != HB_OK) return r;
    return hb_flags_write_operand_value(ctx, &instr->src2, old);
}

static hb_result_t hb_jit_atomic_xadd(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_size_t size = instr->src1.size ? instr->src1.size : instr->src2.size;
    void* ptr;
    uint64_t addr = 0;
    uint64_t src_val = 0;
    uint64_t old = 0;
    uint64_t result;
    hb_result_t r;

    if (!size) size = HB_SIZE_32;
    ptr = hb_jit_atomic_host_ptr(ctx, &instr->src1, size, &addr);
    if (!ptr) {
        if (hb_jit_trace_atomics_enabled())
            fprintf(stderr,
                    "macrunner-hb-atomic-xadd: fallback pc=0x%llx instr=0x%llx size=%u\n",
                    (unsigned long long)(ctx ? ctx->pc : 0),
                    (unsigned long long)(instr ? instr->guest_addr : 0),
                    (unsigned)size);
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    src_val = hb_jit_trunc_to_size(src_val, size);

    switch (size) {
        case HB_SIZE_8:  old = __atomic_fetch_add((uint8_t*)ptr, (uint8_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_16: old = __atomic_fetch_add((uint16_t*)ptr, (uint16_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_32: old = __atomic_fetch_add((uint32_t*)ptr, (uint32_t)src_val, __ATOMIC_SEQ_CST); break;
        case HB_SIZE_64:
        default:         old = __atomic_fetch_add((uint64_t*)ptr, src_val, __ATOMIC_SEQ_CST); break;
    }
    old = hb_jit_trunc_to_size(old, size);
    result = hb_jit_trunc_to_size(old + src_val, size);
    if (hb_jit_trace_atomics_enabled()) {
        uint64_t count = __atomic_add_fetch(&hb_jit_atomic_xadd_trace_count, 1, __ATOMIC_RELAXED);
        if (count <= 32 || (count % 200000u) == 0) {
            fprintf(stderr,
                    "macrunner-hb-atomic-xadd: count=%llu pc=0x%llx instr=0x%llx addr=0x%llx ptr=%p size=%u src=%llu old=%llu result=%llu\n",
                    (unsigned long long)count,
                    (unsigned long long)(ctx ? ctx->pc : 0),
                    (unsigned long long)instr->guest_addr,
                    (unsigned long long)addr,
                    ptr,
                    (unsigned)size,
                    (unsigned long long)src_val,
                    (unsigned long long)old,
                    (unsigned long long)result);
        }
    }
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, size, old, src_val, result, 0);
    return hb_flags_write_operand_value(ctx, &instr->src2, old);
}

static hb_result_t hb_jit_atomic_xadd_split_locked(hb_context_t* ctx,
                                                   const hb_ir_instr_t* instr) {
    hb_size_t size = instr->src1.size ? instr->src1.size : instr->src2.size;
    uint64_t src_val = 0;
    uint64_t addr = 0;
    uint64_t old = 0;
    uint64_t result;
    hb_result_t r;

    if (!size) size = HB_SIZE_32;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &src_val);
    if (r != HB_OK) return r;
    src_val = hb_jit_trunc_to_size(src_val, size);
    r = hb_jit_atomic_read_mem_value(ctx, &instr->src1, size, &addr, &old);
    if (r != HB_OK) return r;
    result = hb_jit_trunc_to_size(old + src_val, size);
    r = hb_jit_atomic_write_mem_value(ctx, addr, size, result);
    if (r != HB_OK) return r;
    if (hb_jit_trace_atomics_enabled()) {
        uint64_t count = __atomic_add_fetch(&hb_jit_atomic_xadd_trace_count, 1, __ATOMIC_RELAXED);
        if (count <= 32 || (count % 10000u) == 0) {
            fprintf(stderr,
                    "macrunner-hb-atomic-xadd-split: count=%llu pc=0x%llx instr=0x%llx addr=0x%llx size=%u src=%llu old=%llu result=%llu\n",
                    (unsigned long long)count,
                    (unsigned long long)(ctx ? ctx->pc : 0),
                    (unsigned long long)instr->guest_addr,
                    (unsigned long long)addr,
                    (unsigned)size,
                    (unsigned long long)src_val,
                    (unsigned long long)old,
                    (unsigned long long)result);
        }
    }
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, size, old, src_val, result, 0);
    return hb_flags_write_operand_value(ctx, &instr->src2, old);
}

static hb_result_t hb_jit_atomic_cmpxchg8b(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    void* ptr;
    uint64_t acc, src, old;
    bool equal;

    if (instr->dst.size == HB_SIZE_128)
        return HB_ERR_UNSUPPORTED_FEATURE;
    ptr = hb_jit_atomic_host_ptr(ctx, &instr->dst, HB_SIZE_64, NULL);
    if (!ptr) return HB_ERR_UNSUPPORTED_FEATURE;

    acc = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
          (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
    src = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RCX) << 32) |
          (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RBX);
    old = acc;
    equal = __atomic_compare_exchange_n((uint64_t*)ptr, &old, src,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    hb_lazy_flags_clear(ctx);
    ctx->flags.zf = equal;
    if (!equal) {
        hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)old, HB_SIZE_32);
        hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(old >> 32), HB_SIZE_32);
    }
    return HB_OK;
}

static hb_result_t hb_jit_atomic_cmpxchg8b_split_locked(hb_context_t* ctx,
                                                        const hb_ir_instr_t* instr) {
    uint64_t addr = 0;
    uint64_t acc, src, old = 0;
    bool equal;
    hb_result_t r;

    if (instr->dst.size == HB_SIZE_128) {
        uint64_t mem[2] = {0, 0};
        uint64_t desired[2];

        if (!ctx || !ctx->memory || instr->dst.type != HB_OP_MEM)
            return HB_ERR_UNSUPPORTED_FEATURE;
        addr = hb_jit_resolve_addr(ctx, &instr->dst);
        if (addr & 0xf)
            return HB_ERR_MEMORY_FAULT;
        r = hb_jit_helper_read_bytes_tso(ctx, addr, mem, sizeof(mem));
        if (r != HB_OK) return r;

        acc = hb_context_read_reg_value(ctx, HB_REG_RAX);
        src = hb_context_read_reg_value(ctx, HB_REG_RDX);
        equal = mem[0] == acc && mem[1] == src;
        if (equal) {
            desired[0] = hb_context_read_reg_value(ctx, HB_REG_RBX);
            desired[1] = hb_context_read_reg_value(ctx, HB_REG_RCX);
            r = hb_jit_helper_write_bytes_tso(ctx, addr, desired, sizeof(desired));
            if (r != HB_OK) return r;
        } else {
            hb_context_write_reg_value_sized(ctx, HB_REG_RAX, mem[0], HB_SIZE_64);
            hb_context_write_reg_value_sized(ctx, HB_REG_RDX, mem[1], HB_SIZE_64);
        }
        hb_lazy_flags_clear(ctx);
        ctx->flags.zf = equal;
        return HB_OK;
    }
    r = hb_jit_atomic_read_mem_value(ctx, &instr->dst, HB_SIZE_64, &addr, &old);
    if (r != HB_OK) return r;

    acc = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
          (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
    src = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RCX) << 32) |
          (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RBX);
    equal = old == acc;
    if (equal) {
        r = hb_jit_atomic_write_mem_value(ctx, addr, HB_SIZE_64, src);
        if (r != HB_OK) return r;
    } else {
        hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)old, HB_SIZE_32);
        hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(old >> 32), HB_SIZE_32);
    }
    hb_lazy_flags_clear(ctx);
    ctx->flags.zf = equal;
    return HB_OK;
}

void hb_jit_helper_exec_atomic_ir(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_result_t r = HB_ERR_UNSUPPORTED_FEATURE;
    uint64_t trace_count = 0;

    if (!ctx || !instr) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }

    if (hb_jit_trace_atomics_enabled()) {
        trace_count = __atomic_add_fetch(&hb_jit_atomic_helper_trace_count, 1, __ATOMIC_RELAXED);
        if (trace_count <= 64 || (trace_count % 200000u) == 0) {
            fprintf(stderr,
                    "macrunner-hb-atomic-helper: enter count=%llu op=%u pc=0x%llx instr=0x%llx dst=%u src1=%u src2=%u\n",
                    (unsigned long long)trace_count,
                    (unsigned)instr->op,
                    (unsigned long long)ctx->pc,
                    (unsigned long long)instr->guest_addr,
                    (unsigned)instr->dst.type,
                    (unsigned)instr->src1.type,
                    (unsigned)instr->src2.type);
        }
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    switch (instr->op) {
        case HB_IR_CMPXCHG:   r = hb_jit_atomic_cmpxchg(ctx, instr); break;
        case HB_IR_CMPXCHG8B: r = hb_jit_atomic_cmpxchg8b(ctx, instr); break;
        case HB_IR_XCHG:      r = hb_jit_atomic_xchg(ctx, instr); break;
        case HB_IR_XADD:      r = hb_jit_atomic_xadd(ctx, instr); break;
        default:              r = HB_ERR_UNSUPPORTED_OPCODE; break;
    }

    if (r == HB_ERR_UNSUPPORTED_FEATURE) {
        hb_jit_split_lock_acquire();
        switch (instr->op) {
            case HB_IR_CMPXCHG:
                r = hb_jit_atomic_cmpxchg_split_locked(ctx, instr);
                break;
            case HB_IR_CMPXCHG8B:
                r = hb_jit_atomic_cmpxchg8b_split_locked(ctx, instr);
                break;
            case HB_IR_XCHG:
                r = hb_jit_atomic_xchg_split_locked(ctx, instr);
                break;
            case HB_IR_XADD:
                r = hb_jit_atomic_xadd_split_locked(ctx, instr);
                break;
            default:
                r = hb_interpreter_exec_one_for_jit(ctx, instr);
                break;
        }
        hb_jit_split_lock_release();
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (trace_count && (trace_count <= 64 || (trace_count % 200000u) == 0)) {
        fprintf(stderr,
                "macrunner-hb-atomic-helper: exit count=%llu op=%u result=%d pc=0x%llx instr=0x%llx\n",
                (unsigned long long)trace_count,
                (unsigned)instr->op,
                (int)r,
                (unsigned long long)ctx->pc,
                (unsigned long long)instr->guest_addr);
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_result_t r = HB_OK;
    uint64_t lhs = 0, rhs = 0;
    hb_size_t size;
    if (!ctx || !instr) return;

    switch (instr->op) {
        case HB_IR_IMUL:
            if (instr->dst.type == HB_OP_NONE) {
                r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
                if (r != HB_OK) break;
                hb_jit_helper_acquire_after_operand_read(&instr->src1);
                size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
                if (size == HB_SIZE_8) {
                    int16_t result = (int16_t)((int8_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int8_t)rhs);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT8_MIN || result > INT8_MAX);
                } else if (size == HB_SIZE_16) {
                    int32_t result = (int32_t)((int16_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int16_t)rhs);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT16_MIN || result > INT16_MAX);
                } else if (size == HB_SIZE_32) {
                    int64_t result = (int64_t)(int32_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int64_t)(int32_t)rhs;
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                    ctx->flags.cf = ctx->flags.of = (result < INT32_MIN || result > INT32_MAX);
                } else if (size == HB_SIZE_64) {
                    __int128 result = (__int128)(int64_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (__int128)(int64_t)rhs;
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                    ctx->flags.cf = ctx->flags.of = (result < (__int128)INT64_MIN || result > (__int128)INT64_MAX);
                } else r = HB_ERR_UNSUPPORTED_OPCODE;
                hb_lazy_flags_clear(ctx);
                break;
            }
            r = hb_flags_read_operand_value(ctx, &instr->src1, &lhs);
            if (r != HB_OK) break;
            hb_jit_helper_acquire_after_operand_read(&instr->src1);
            r = hb_flags_read_operand_value(ctx, &instr->src2, &rhs);
            if (r != HB_OK) break;
            hb_jit_helper_acquire_after_operand_read(&instr->src2);
            size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, hb_jit_trunc_to_size(lhs * rhs, size), size);
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_MUL:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            hb_jit_helper_acquire_after_operand_read(&instr->src1);
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (size == HB_SIZE_8) {
                uint16_t result = (uint16_t)(uint8_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint16_t)(uint8_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, result, HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 8) != 0);
            } else if (size == HB_SIZE_16) {
                uint32_t result = (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint32_t)(uint16_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 16) != 0);
            } else if (size == HB_SIZE_32) {
                uint64_t result = (uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint64_t)(uint32_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                ctx->flags.cf = ctx->flags.of = ((result >> 32) != 0);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 result = (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX) * (unsigned __int128)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                ctx->flags.cf = ctx->flags.of = ((uint64_t)(result >> 64) != 0);
            } else r = HB_ERR_UNSUPPORTED_OPCODE;
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_DIV:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            hb_jit_helper_acquire_after_operand_read(&instr->src1);
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (!rhs) { r = HB_ERR_EXEC_FAULT; break; }
            if (size == HB_SIZE_8) {
                uint16_t dividend = (uint16_t)(hb_context_read_reg_value(ctx, HB_REG_RAX) & 0xffffu);
                uint16_t quotient = dividend / (uint8_t)rhs;
                uint16_t remainder = dividend % (uint8_t)rhs;
                if (quotient > UINT8_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient, HB_SIZE_16);
            } else if (size == HB_SIZE_16) {
                uint32_t dividend = ((uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 16) |
                                    (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                uint32_t quotient = dividend / (uint16_t)rhs;
                uint32_t remainder = dividend % (uint16_t)rhs;
                if (quotient > UINT16_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)quotient, HB_SIZE_16);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)remainder, HB_SIZE_16);
            } else if (size == HB_SIZE_32) {
                uint64_t dividend = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
                                    (uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                uint64_t quotient = dividend / (uint32_t)rhs;
                uint64_t remainder = dividend % (uint32_t)rhs;
                if (quotient > UINT32_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)quotient, HB_SIZE_32);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)remainder, HB_SIZE_32);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 dividend = ((unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RDX) << 64) |
                                             (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX);
                unsigned __int128 quotient = dividend / rhs;
                unsigned __int128 remainder = dividend % rhs;
                if (quotient > UINT64_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
            } else r = HB_ERR_UNSUPPORTED_OPCODE;
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_IDIV:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            hb_jit_helper_acquire_after_operand_read(&instr->src1);
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (!rhs) { r = HB_ERR_EXEC_FAULT; break; }
            {
                int64_t divisor = hb_jit_sign_extend_from_size(rhs, size);
                if (!divisor) { r = HB_ERR_EXEC_FAULT; break; }
                if (size == HB_SIZE_8) {
                    int16_t dividend = (int16_t)(hb_context_read_reg_value(ctx, HB_REG_RAX) & 0xffffu);
                    int64_t quotient = dividend / (int8_t)divisor;
                    int64_t remainder = dividend % (int8_t)divisor;
                    if (quotient < INT8_MIN || quotient > INT8_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient, HB_SIZE_16);
                } else if (size == HB_SIZE_16) {
                    int32_t dividend = (int32_t)(((uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 16) |
                                                 (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX));
                    int64_t quotient = dividend / (int16_t)divisor;
                    int64_t remainder = dividend % (int16_t)divisor;
                    if (quotient < INT16_MIN || quotient > INT16_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)quotient, HB_SIZE_16);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)remainder, HB_SIZE_16);
                } else if (size == HB_SIZE_32) {
                    int64_t dividend = ((int64_t)(int32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
                                       (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                    int64_t quotient = dividend / (int32_t)divisor;
                    int64_t remainder = dividend % (int32_t)divisor;
                    if (quotient < INT32_MIN || quotient > INT32_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)quotient, HB_SIZE_32);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)remainder, HB_SIZE_32);
                } else if (size == HB_SIZE_64) {
                    unsigned __int128 bits = ((unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RDX) << 64) |
                                             (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX);
                    __int128 dividend = (__int128)bits;
                    __int128 quotient = dividend / (int64_t)divisor;
                    __int128 remainder = dividend % (int64_t)divisor;
                    if (quotient < (__int128)INT64_MIN || quotient > (__int128)INT64_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
                } else r = HB_ERR_UNSUPPORTED_OPCODE;
            }
            hb_lazy_flags_clear(ctx);
            break;

        default:
            r = HB_ERR_UNSUPPORTED_OPCODE;
            break;
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t count = 0;
    uint64_t next = 0;
    bool taken = false;
    hb_result_t r;
    hb_size_t size;
    if (!ctx || !instr) return;
    size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
    r = hb_flags_read_operand_value(ctx, &instr->dst, &count);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    hb_jit_helper_acquire_after_operand_read(&instr->dst);
    count = hb_jit_trunc_to_size(count, size);
    if (instr->op == HB_IR_JRCXZ) {
        taken = count == 0;
    } else {
        int kind = (int)instr->src1.imm;
        next = hb_jit_trunc_to_size(count - 1, size);
        hb_jit_helper_release_before_operand_write(&instr->dst);
        r = hb_flags_write_operand_value(ctx, &instr->dst, next);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
        taken = next != 0 &&
                (kind == 2 || (kind == 1 ? ctx->flags.zf : !ctx->flags.zf));
    }
    ctx->pc = taken ? instr->target : instr->guest_addr + instr->guest_len;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eip = (uint32_t)ctx->pc;
    else ctx->regs.x64.rip = ctx->pc;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    ctx->last_result = hb_interpreter_exec_one_for_jit(ctx, instr);
}

static hb_result_t hb_jit_helper_exec_block_instr_for_jit(hb_context_t* ctx,
                                                          const hb_ir_instr_t* instr) {
    switch (instr->op) {
        case HB_IR_CMPXCHG:
        case HB_IR_CMPXCHG8B:
        case HB_IR_XCHG:
        case HB_IR_XADD:
            if (hb_jit_trace_atomics_enabled()) {
                uint64_t count = __atomic_add_fetch(&hb_jit_block_atomic_trace_count, 1, __ATOMIC_RELAXED);
                if (count <= 128 || (count % 100000u) == 0) {
                    fprintf(stderr,
                            "macrunner-hb-block-atomic: enter count=%llu op=%u pc=0x%llx instr=0x%llx dst=%u src1=%u src2=%u\n",
                            (unsigned long long)count,
                            (unsigned)instr->op,
                            (unsigned long long)(ctx ? ctx->pc : 0),
                            (unsigned long long)instr->guest_addr,
                            (unsigned)instr->dst.type,
                            (unsigned)instr->src1.type,
                            (unsigned)instr->src2.type);
                }
            }
            hb_jit_helper_exec_atomic_ir(ctx, instr);
            if (hb_jit_trace_atomics_enabled()) {
                uint64_t count = __atomic_load_n(&hb_jit_block_atomic_trace_count, __ATOMIC_RELAXED);
                if (count <= 128 || (count % 100000u) == 0) {
                    fprintf(stderr,
                            "macrunner-hb-block-atomic: exit count=%llu op=%u result=%d pc=0x%llx instr=0x%llx\n",
                            (unsigned long long)count,
                            (unsigned)instr->op,
                            (int)(ctx ? ctx->last_result : HB_ERR_INVALID_ARG),
                            (unsigned long long)(ctx ? ctx->pc : 0),
                            (unsigned long long)instr->guest_addr);
                }
            }
            return ctx->last_result;
        default:
            return hb_interpreter_exec_one_for_jit(ctx, instr);
    }
}

static bool hb_jit_helper_is_control_transfer(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

static uint64_t hb_jit_helper_two_block_loop_budget(void) {
    static uint64_t cached = 0;
    uint64_t v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (!v) {
        const char* env = getenv("MACRUNNER_HB_JIT_HELPER_LOOP_BLOCK_BUDGET");
        v = (env && *env) ? strtoull(env, NULL, 0) : 16384ULL;
        if (!v) v = 16384ULL;
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}

static uint64_t hb_jit_helper_trace_loop_pc(void) {
    static int parsed;
    static uint64_t pc;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_HELPER_LOOP_PC");
        if (env && *env) pc = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return pc;
}

static void hb_jit_helper_trace_two_block_loop(const char* phase, hb_context_t* ctx,
                                               const hb_ir_block_t* first,
                                               const hb_ir_block_t* second,
                                               uint64_t blocks,
                                               uint64_t budget) {
    static unsigned reports;
    uint64_t watch = hb_jit_helper_trace_loop_pc();
    if (!watch || !ctx || !first || !second) return;
    if (watch != ctx->pc && watch != first->guest_addr && watch != second->guest_addr) return;
    if (reports >= 96 && strcmp(phase, "budget") != 0) return;
    reports++;
    if (ctx->mode == HB_MODE_32BIT) {
        fprintf(stderr,
                "macrunner-hb-helper-loop: phase=%s watch=%p pc=%p first=%p second=%p "
                "blocks=%llu budget=%llu eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x "
                "esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x lazy{pending=%u kind=%u width=%u lhs=%llx rhs=%llx result=%llx}\n",
                phase ? phase : "?", (void*)(uintptr_t)watch, (void*)(uintptr_t)ctx->pc,
                (void*)(uintptr_t)first->guest_addr, (void*)(uintptr_t)second->guest_addr,
                (unsigned long long)blocks, (unsigned long long)budget,
                ctx->regs.x86.eip, ctx->regs.x86.eax, ctx->regs.x86.ebx,
                ctx->regs.x86.ecx, ctx->regs.x86.edx, ctx->regs.x86.esi,
                ctx->regs.x86.edi, ctx->regs.x86.ebp, ctx->regs.x86.esp,
                ctx->regs.x86.eflags, (unsigned)ctx->lazy_flags.pending,
                (unsigned)ctx->lazy_flags.kind, (unsigned)ctx->lazy_flags.width,
                (unsigned long long)ctx->lazy_flags.lhs,
                (unsigned long long)ctx->lazy_flags.rhs,
                (unsigned long long)ctx->lazy_flags.result);
    } else {
        fprintf(stderr,
                "macrunner-hb-helper-loop: phase=%s watch=%p pc=%p first=%p second=%p "
                "blocks=%llu budget=%llu rip=%p rax=%p rcx=%p rsi=%p rdi=%p eflags=%08x\n",
                phase ? phase : "?", (void*)(uintptr_t)watch, (void*)(uintptr_t)ctx->pc,
                (void*)(uintptr_t)first->guest_addr, (void*)(uintptr_t)second->guest_addr,
                (unsigned long long)blocks, (unsigned long long)budget,
                (void*)(uintptr_t)ctx->regs.x64.rip, (void*)(uintptr_t)ctx->regs.x64.rax,
                (void*)(uintptr_t)ctx->regs.x64.rcx, (void*)(uintptr_t)ctx->regs.x64.rsi,
                (void*)(uintptr_t)ctx->regs.x64.rdi, (unsigned)ctx->regs.x64.rflags);
    }
    fflush(stderr);
}

static void hb_jit_helper_sync_pc(hb_context_t* ctx) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eip = (uint32_t)ctx->pc;
    else ctx->regs.x64.rip = ctx->pc;
}

static hb_result_t hb_jit_helper_exec_ir_block_once(hb_context_t* ctx,
                                                    const hb_ir_block_t* block) {
    if (!ctx || !block) return HB_ERR_INVALID_ARG;
    for (size_t i = 0; i < block->instr_count; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        hb_result_t r = hb_jit_helper_exec_block_instr_for_jit(ctx, instr);
        if (r != HB_OK) return r;
        if (hb_jit_helper_is_control_transfer(instr->op)) return HB_OK;
    }
    if (block->instr_count) {
        const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
        ctx->pc = last->guest_addr + last->guest_len;
        hb_jit_helper_sync_pc(ctx);
    }
    return HB_OK;
}

void hb_jit_helper_exec_two_block_loop(hb_context_t* ctx,
                                       const hb_ir_block_t* first,
                                       const hb_ir_block_t* second) {
    uint64_t budget = hb_jit_helper_two_block_loop_budget();
    uint64_t blocks = 0;
    if (!ctx || !first || !second) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    ctx->last_result = HB_OK;
    while (blocks++ < budget) {
        const hb_ir_block_t* block = NULL;
        if (blocks <= 8) hb_jit_helper_trace_two_block_loop("before", ctx, first, second,
                                                            blocks, budget);
        if (ctx->pc == first->guest_addr) block = first;
        else if (ctx->pc == second->guest_addr) block = second;
        else return;

        hb_result_t r = hb_jit_helper_exec_ir_block_once(ctx, block);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
        if (blocks <= 8) hb_jit_helper_trace_two_block_loop("after", ctx, first, second,
                                                           blocks, budget);
        if (ctx->pc != first->guest_addr && ctx->pc != second->guest_addr) return;
    }
    /* Leave ctx->pc inside the loop; the runtime will re-enter the cached loop block. */
    hb_jit_helper_trace_two_block_loop("budget", ctx, first, second, blocks - 1, budget);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_four_block_loop(hb_context_t* ctx,
                                        const hb_ir_block_t* first,
                                        const hb_ir_block_t* second,
                                        const hb_ir_block_t* third,
                                        const hb_ir_block_t* fourth) {
    uint64_t budget = hb_jit_helper_two_block_loop_budget();
    uint64_t blocks = 0;
    if (!ctx || !first || !second || !third) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    ctx->last_result = HB_OK;
    while (blocks++ < budget) {
        const hb_ir_block_t* block = NULL;
        if (ctx->pc == first->guest_addr) block = first;
        else if (ctx->pc == second->guest_addr) block = second;
        else if (ctx->pc == third->guest_addr) block = third;
        else if (fourth && ctx->pc == fourth->guest_addr) block = fourth;
        else return;

        hb_result_t r = hb_jit_helper_exec_ir_block_once(ctx, block);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
        if (ctx->pc != first->guest_addr && ctx->pc != second->guest_addr &&
            ctx->pc != third->guest_addr && (!fourth || ctx->pc != fourth->guest_addr))
            return;
    }
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_ir_block(hb_context_t* ctx, const hb_ir_block_t* block) {
    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
}

static hb_result_t hb_jit_helper_exec_cmp_or_test(hb_context_t* ctx,
                                                  const hb_ir_instr_t* instr) {
    uint64_t lhs = 0;
    uint64_t rhs = 0;
    hb_result_t r = hb_flags_read_operand_value(ctx, &instr->src1, &lhs);
    if (r != HB_OK) return r;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    r = hb_flags_read_operand_value(ctx, &instr->src2, &rhs);
    if (r != HB_OK) return r;
    hb_jit_helper_acquire_after_operand_read(&instr->src2);
    if (instr->op == HB_IR_CMP) {
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, instr->src1.size, lhs, rhs, lhs - rhs, 0);
        return HB_OK;
    }
    if (instr->op == HB_IR_TEST) {
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, instr->src1.size, lhs, rhs, lhs & rhs, 0);
        return HB_OK;
    }
    return HB_ERR_INVALID_ARG;
}

static hb_result_t hb_jit_helper_exec_ret_exact(hb_context_t* ctx,
                                                const hb_ir_instr_t* instr) {
    uint64_t ret_addr = 0;
    uint64_t ret_imm = instr->src1.type == HB_OP_IMM ? (uint64_t)instr->src1.imm : 0;
    hb_result_t r;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t ret32 = 0;
        r = hb_jit_helper_read_u32_tso(ctx, ctx->regs.x86.esp, &ret32);
        if (r != HB_OK) return r;
        ret_addr = ret32;
        ctx->regs.x86.esp += 4 + (uint32_t)ret_imm;
    } else {
        r = hb_jit_helper_read_u64_tso(ctx, ctx->regs.x64.rsp, &ret_addr);
        if (r != HB_OK) return r;
        ctx->regs.x64.rsp += 8 + ret_imm;
    }
    if (!ret_addr) {
        /* MacRunner diag: the guest RET'd to a NULL return address -> pc=0 ->
         * native-dispatched -> execute-at-0. Log the RET site (the function that
         * returned to 0) + regs. ret_addr==0 is rare (corrupted return). */
        static int retn;
        if (retn++ < 16)
            fprintf(stderr, "macrunner-hb-jit-retnull: ret_site_guest_pc=0x%llx rsp_after=0x%llx "
                    "ret_imm=%llu rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rbp=0x%llx "
                    "rsi=0x%llx rdi=0x%llx\n",
                    (unsigned long long)instr->guest_addr, (unsigned long long)ctx->regs.x64.rsp,
                    (unsigned long long)ret_imm, (unsigned long long)ctx->regs.x64.rax,
                    (unsigned long long)ctx->regs.x64.rcx, (unsigned long long)ctx->regs.x64.rdx,
                    (unsigned long long)ctx->regs.x64.rbx, (unsigned long long)ctx->regs.x64.rbp,
                    (unsigned long long)ctx->regs.x64.rsi, (unsigned long long)ctx->regs.x64.rdi);
        fflush(stderr);
    }
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    return HB_OK;
}

void hb_jit_helper_exec_load_cmp_jcc_block(hb_context_t* ctx, const hb_ir_block_t* block) {
    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !hot_helper_load_cmp_jcc_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* cmp = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    hb_ir_operand_t load_src = load->src1;
    load_src.size = load->dst.size;

    uint64_t value = 0;
    hb_result_t r = hb_flags_read_operand_value(ctx, &load_src, &value);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&load_src);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &load->dst, value);
    if (r == HB_OK) r = hb_jit_helper_exec_cmp_or_test(ctx, cmp);
    bool taken = false;
    if (r == HB_OK) r = hb_flags_eval_cond(ctx, jcc->cc, &taken);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    ctx->pc = taken ? jcc->target : jcc->guest_addr + jcc->guest_len;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_cmp_setcc_ret_block(hb_context_t* ctx, const hb_ir_block_t* block) {
    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !hot_helper_cmp_setcc_ret_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    const hb_ir_instr_t* cmp = &block->instrs[0];
    const hb_ir_instr_t* setcc = &block->instrs[1];
    const hb_ir_instr_t* ret = &block->instrs[2];
    hb_result_t r = hb_jit_helper_exec_cmp_or_test(ctx, cmp);
    bool value = false;
    if (r == HB_OK) r = hb_flags_eval_cond(ctx, setcc->cc, &value);
    if (r == HB_OK) hb_jit_helper_release_before_operand_write(&setcc->dst);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &setcc->dst, value ? 1 : 0);
    if (r == HB_OK) r = hb_jit_helper_exec_ret_exact(ctx, ret);
    ctx->last_result = r;
}

static hb_result_t hb_jit_helper_read_u8_fast(hb_context_t* ctx, uint64_t addr, uint8_t* out);
static hb_result_t hb_jit_helper_read_u16_fast(hb_context_t* ctx, uint64_t addr, uint16_t* out);
static hb_result_t hb_jit_helper_read_u32_fast(hb_context_t* ctx, uint64_t addr, uint32_t* out);
static hb_result_t hb_jit_helper_read_u64_fast(hb_context_t* ctx, uint64_t addr, uint64_t* out);

void hb_jit_helper_exec_i32_less_tiebreaker(hb_context_t* ctx,
                                            const hb_ir_block_t* entry,
                                            const hb_ir_block_t* equal,
                                            const hb_ir_block_t* less) {
    if (!ctx || !entry || !equal || entry->instr_count != 3 || equal->instr_count != 3) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }

    const hb_ir_instr_t* load = &entry->instrs[0];
    const hb_ir_instr_t* entry_cmp = &entry->instrs[1];
    const hb_ir_instr_t* entry_jcc = &entry->instrs[2];
    const hb_ir_instr_t* equal_cmp = &equal->instrs[0];
    const hb_ir_instr_t* equal_setcc = &equal->instrs[1];
    const hb_ir_instr_t* equal_ret = &equal->instrs[2];
    const hb_ir_instr_t* less_setcc = less && less->instr_count == 2 ? &less->instrs[0] : NULL;
    const hb_ir_instr_t* less_ret = less && less->instr_count == 2 ? &less->instrs[1] : NULL;
    uint64_t lhs = 0;
    uint64_t rhs = 0;
    uint64_t loaded = 0;
    bool taken = false;
    hb_result_t r;

    if (load->op != HB_IR_LOAD || entry_cmp->op != HB_IR_CMP ||
        entry_jcc->op != HB_IR_Jcc || equal_cmp->op != HB_IR_CMP ||
        equal_setcc->op != HB_IR_SETcc || equal_ret->op != HB_IR_RET) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, entry);
        return;
    }

    hb_ir_operand_t load_src = load->src1;
    load_src.size = load->dst.size;
    r = hb_flags_read_operand_value(ctx, &load_src, &loaded);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&load_src);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &load->dst, loaded);
    if (r == HB_OK) r = hb_flags_read_operand_value(ctx, &entry_cmp->src1, &lhs);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&entry_cmp->src1);
    if (r == HB_OK) r = hb_flags_read_operand_value(ctx, &entry_cmp->src2, &rhs);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&entry_cmp->src2);
    if (r == HB_OK) {
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, entry_cmp->src1.size, lhs, rhs, lhs - rhs, 0);
        r = hb_flags_eval_cond(ctx, entry_jcc->cc, &taken);
    }
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }

    if (taken) {
        bool value = false;
        if (!less_setcc || !less_ret || less_setcc->op != HB_IR_SETcc || less_ret->op != HB_IR_RET) {
            ctx->pc = entry_jcc->target;
            hb_jit_helper_sync_pc(ctx);
            ctx->last_result = HB_OK;
            return;
        }
        r = hb_flags_eval_cond(ctx, less_setcc->cc, &value);
        if (r == HB_OK) hb_jit_helper_release_before_operand_write(&less_setcc->dst);
        if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &less_setcc->dst, value ? 1 : 0);
        if (r == HB_OK) r = hb_jit_helper_exec_ret_exact(ctx, less_ret);
        ctx->last_result = r;
        return;
    }

    r = hb_flags_read_operand_value(ctx, &equal_cmp->src1, &lhs);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&equal_cmp->src1);
    if (r == HB_OK) r = hb_flags_read_operand_value(ctx, &equal_cmp->src2, &rhs);
    if (r == HB_OK) hb_jit_helper_acquire_after_operand_read(&equal_cmp->src2);
    if (r == HB_OK) {
        bool value = false;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, equal_cmp->src1.size, lhs, rhs, lhs - rhs, 0);
        r = hb_flags_eval_cond(ctx, equal_setcc->cc, &value);
        if (r == HB_OK) hb_jit_helper_release_before_operand_write(&equal_setcc->dst);
        if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &equal_setcc->dst, value ? 1 : 0);
    }
    if (r == HB_OK) r = hb_jit_helper_exec_ret_exact(ctx, equal_ret);
    ctx->last_result = r;
}

static bool hb_jit_helper_unity_sort_cmp_bytes_ready(hb_context_t* ctx, uint64_t cmp_pc) {
    static const uint8_t unity_cmp_bytes[] = {
        0x8b, 0x02, 0x39, 0x01, 0x75, 0x07, 0x48, 0x3b, 0xca,
        0x0f, 0x92, 0xc0, 0xc3, 0x0f, 0x9c, 0xc0, 0xc3
    };
    uint8_t bytes[sizeof(unity_cmp_bytes)];
    if (!ctx || !ctx->memory || !cmp_pc) return false;
    if (hb_memory_read(ctx->memory, cmp_pc, bytes, sizeof(bytes)) != HB_OK)
        return false;
    return memcmp(bytes, unity_cmp_bytes, sizeof(bytes)) == 0;
}

static bool hb_jit_helper_unity_sort_bytes_ready(hb_context_t* ctx,
                                                 const hb_ir_block_t* sort) {
    static const uint8_t unity_sort_bytes[] = {
        0x48, 0x8b, 0x07,             /* mov rax, [rdi] */
        0x49, 0x8b, 0xcf,             /* mov rcx, r15 */
        0x49, 0x89, 0x06,             /* mov [r14], rax */
        0x4c, 0x8b, 0xf7,             /* mov r14, rdi */
        0x48, 0x8b, 0x57, 0xf8,       /* mov rdx, [rdi-8] */
        0x48, 0x83, 0xef, 0x08,       /* sub rdi, 8 */
        0xff, 0xd5                    /* call rbp */
    };
    uint8_t bytes[sizeof(unity_sort_bytes)];
    if (!ctx || !ctx->memory || !sort) return false;
    if (hb_memory_read(ctx->memory, sort->guest_addr, bytes, sizeof(bytes)) != HB_OK)
        return false;
    return memcmp(bytes, unity_sort_bytes, sizeof(bytes)) == 0;
}

static bool hb_jit_helper_unity_sort_guard_ready(hb_context_t* ctx,
                                                 const hb_ir_block_t* sort,
                                                 uint64_t* fallthrough_pc) {
    uint64_t guard_pc;
    uint8_t bytes[4];
    int8_t rel;
    uint64_t target;
    if (!ctx || !ctx->memory || !sort || sort->instr_count != 7)
        return false;
    guard_pc = sort->instrs[6].guest_addr + sort->instrs[6].guest_len;
    if (hb_memory_read(ctx->memory, guard_pc, bytes, sizeof(bytes)) != HB_OK)
        return false;
    if (bytes[0] != 0x84 || bytes[1] != 0xc0 || bytes[2] != 0x75)
        return false;
    rel = (int8_t)bytes[3];
    target = guard_pc + sizeof(bytes) + rel;
    if (target != sort->guest_addr)
        return false;
    if (fallthrough_pc) *fallthrough_pc = guard_pc + sizeof(bytes);
    return true;
}

void hb_jit_helper_exec_unity_sort_inner_loop(hb_context_t* ctx,
                                              const hb_ir_block_t* sort) {
    uint64_t budget = hb_jit_helper_two_block_loop_budget();
    uint64_t rax, rcx, rdx, rdi, r14, r15, rbp;
    uint64_t fallthrough_pc = 0;
    hb_result_t r = HB_OK;

    if (!ctx || !sort || sort->instr_count != 7) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || ctx->pc != sort->guest_addr ||
        !hb_jit_helper_unity_sort_bytes_ready(ctx, sort) ||
        !hb_jit_helper_unity_sort_cmp_bytes_ready(ctx, ctx->regs.x64.rbp) ||
        !hb_jit_helper_unity_sort_guard_ready(ctx, sort, &fallthrough_pc)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, sort);
        return;
    }

    rax = ctx->regs.x64.rax;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    rdi = ctx->regs.x64.rdi;
    r14 = ctx->regs.x64.r14;
    r15 = ctx->regs.x64.r15;
    rbp = ctx->regs.x64.rbp;

    for (uint64_t iter = 0; iter < budget; iter++) {
        uint32_t lhs = 0;
        uint32_t rhs = 0;
        uint8_t al;

        r = hb_jit_helper_read_u64_fast(ctx, rdi, &rax);
        if (r != HB_OK) break;
        rcx = r15;
        r = hb_jit_helper_write_u64_tso(ctx, r14, rax);
        if (r != HB_OK) break;
        r14 = rdi;
        r = hb_jit_helper_read_u64_fast(ctx, rdi - 8u, &rdx);
        if (r != HB_OK) break;
        rdi -= 8u;

        r = hb_jit_helper_read_u32_fast(ctx, rdx, &rhs);
        if (r != HB_OK) break;
        r = hb_jit_helper_read_u32_fast(ctx, rcx, &lhs);
        if (r != HB_OK) break;
        rax = rhs;
        if (lhs != rhs) {
            al = ((int32_t)lhs < (int32_t)rhs) ? 1u : 0u;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_32,
                               lhs, rhs, (uint32_t)(lhs - rhs), 0);
        } else {
            al = (rcx < rdx) ? 1u : 0u;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64,
                               rcx, rdx, rcx - rdx, 0);
        }
        rax = (rax & ~0xffull) | al;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, HB_SIZE_8, al, al, al, 0);
        if (al) continue;

        ctx->pc = fallthrough_pc;
        ctx->regs.x64.rip = ctx->pc;
        ctx->regs.x64.rax = rax;
        ctx->regs.x64.rcx = rcx;
        ctx->regs.x64.rdx = rdx;
        ctx->regs.x64.rdi = rdi;
        ctx->regs.x64.r14 = r14;
        ctx->regs.x64.r15 = r15;
        ctx->regs.x64.rbp = rbp;
        ctx->last_result = HB_OK;
        return;
    }

    ctx->regs.x64.rax = rax;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.rdi = rdi;
    ctx->regs.x64.r14 = r14;
    ctx->regs.x64.r15 = r15;
    ctx->regs.x64.rbp = rbp;
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    ctx->pc = sort->guest_addr;
    ctx->regs.x64.rip = ctx->pc;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_unity_string_bsearch_loop(hb_context_t* ctx,
                                                  const hb_ir_block_t* block) {
    uint64_t rax, rbx, rcx, rdx, r8, r9, r10, r11, rdi;
    hb_result_t r = HB_OK;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !unity_string_bsearch_loop_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rax = ctx->regs.x64.rax;
    rbx = ctx->regs.x64.rbx;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    r8 = ctx->regs.x64.r8;
    r9 = ctx->regs.x64.r9;
    r10 = ctx->regs.x64.r10;
    r11 = ctx->regs.x64.r11;
    rdi = ctx->regs.x64.rdi;

    for (uint64_t iter = 0; (int64_t)r9 > 0; iter++) {
        if (iter >= 4096) {
            r = HB_ERR_EXEC_FAULT;
            break;
        }

        r10 = r9 >> 1;
        r11 = r10;
        r11 += r11;
        r = hb_jit_read_guest_u64_result(ctx, rbx + r11 * 8, &rax);
        if (r != HB_OK) break;
        r8 = rdi - rax;

        for (uint64_t scan = 0;; scan++) {
            uint8_t lhs = 0;
            uint8_t rhs = 0;
            uint32_t diff;
            if (scan >= (1u << 20)) {
                r = HB_ERR_EXEC_FAULT;
                break;
            }
            r = hb_jit_read_guest_u8_result(ctx, rax, &lhs);
            if (r != HB_OK) break;
            r = hb_jit_read_guest_u8_result(ctx, rax + r8, &rhs);
            if (r != HB_OK) break;
            rcx = rhs;
            diff = (uint32_t)lhs - (uint32_t)rhs;
            rdx = diff;
            if (diff != 0) break;
            rax++;
            if ((uint32_t)rcx == 0) break;
        }
        if (r != HB_OK) break;

        if ((int32_t)(uint32_t)rdx < 0) {
            rbx = rbx + r11 * 8 + 0x10;
            rax = UINT64_MAX - r10;
            r9 += rax;
        } else {
            r9 = r10;
        }
    }

    ctx->regs.x64.rax = rax;
    ctx->regs.x64.rbx = rbx;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.r8 = r8;
    ctx->regs.x64.r9 = r9;
    ctx->regs.x64.r10 = r10;
    ctx->regs.x64.r11 = r11;

    if (r == HB_OK) {
        ctx->pc = block->guest_addr + 0x55;
        ctx->regs.x64.rip = ctx->pc;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, HB_SIZE_64, r9, r9, r9, 0);
    }
	    ctx->last_result = r;
}

void hb_jit_helper_exec_unity_freelist_fill_loop(hb_context_t* ctx,
                                                 const hb_ir_block_t* block) {
    size_t prefix_count = 0;
    const hb_ir_instr_t* body = NULL;
    uint64_t loop_pc, exit_pc;
    uint64_t rcx, rdx, rsi, r8, r14, old_head;
    uint64_t budget = hb_jit_helper_two_block_loop_budget();
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    body = unity_freelist_fill_loop_instrs(block, &prefix_count);
    if (ctx->mode != HB_MODE_64BIT || !body || !unity_freelist_fill_loop_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    loop_pc = body[0].guest_addr;
    exit_pc = loop_pc + 0x7bu;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    rsi = ctx->regs.x64.rsi;
    r8 = ctx->regs.x64.r8;
    r14 = ctx->regs.x64.r14;
    if (prefix_count) {
        r8 = (uint64_t)(int64_t)(int32_t)r8;
        ctx->regs.x64.r8 = r8;
    }

    for (uint64_t iter = 0; iter < budget; iter++) {
        ctx->regs.x64.rax = rcx;
        r = hb_jit_helper_write_u64_tso(ctx, rcx, r14);
        if (r != HB_OK) { ctx->last_result = r; return; }

        r = hb_jit_helper_exchange_u64_tso(ctx, rsi + 0x80u, rcx, &old_head);
        if (r != HB_OK) { ctx->last_result = r; return; }
        ctx->regs.x64.rax = old_head;

        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, HB_SIZE_64,
                           old_head, old_head, old_head, 0);
        if (old_head) {
            r = hb_jit_helper_write_u64_tso(ctx, old_head, rcx);
        } else {
            r = hb_jit_helper_write_u64_tso(ctx, rsi + 0x40u, rcx);
        }
        if (r != HB_OK) { ctx->last_result = r; return; }

        rcx += r8;
        ctx->regs.x64.rcx = rcx;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64,
                           rcx, rdx, rcx - rdx, 0);
        if (rcx > rdx) {
            ctx->pc = exit_pc;
            hb_jit_helper_sync_pc(ctx);
            ctx->last_result = HB_OK;
            return;
        }
    }

    ctx->pc = loop_pc;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

static hb_result_t hb_jit_helper_read_u8_fast(hb_context_t* ctx, uint64_t addr, uint8_t* out);
static hb_result_t hb_jit_helper_read_u16_fast(hb_context_t* ctx, uint64_t addr, uint16_t* out);
static hb_result_t hb_jit_helper_read_u32_fast(hb_context_t* ctx, uint64_t addr, uint32_t* out);
static hb_result_t hb_jit_helper_read_u64_fast(hb_context_t* ctx, uint64_t addr, uint64_t* out);

void hb_jit_helper_exec_unity_u32_ptr_compare(hb_context_t* ctx,
                                               const hb_ir_block_t* block) {
    uint64_t rsp, ret_addr, rcx, rdx;
    uint32_t lhs = 0;
    uint32_t rhs = 0;
    uint64_t rax;
    uint8_t al;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !unity_u32_ptr_compare_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rsp = ctx->regs.x64.rsp;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, rdx, &rhs);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, rcx, &lhs);
    if (r != HB_OK) { ctx->last_result = r; return; }

    rax = rhs;
    if (lhs != rhs) {
        al = ((int32_t)lhs < (int32_t)rhs) ? 1u : 0u;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_32,
                           lhs, rhs, (uint32_t)(lhs - rhs), 0);
    } else {
        al = (rcx < rdx) ? 1u : 0u;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64,
                           rcx, rdx, rcx - rdx, 0);
    }

    ctx->regs.x64.rax = (rax & ~0xffull) | al;
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

static const uint8_t* hb_jit_helper_host_read_span(hb_context_t* ctx, uint64_t addr,
                                                   size_t min_size, size_t* available) {
    hb_region_t* region;
    uint64_t region_end;
    if (available) *available = 0;
    if (!ctx || !ctx->memory) return NULL;
    if (min_size && addr + min_size < addr) return NULL;
    if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-host_read_span: addr=0x%llx min=%zu\n", (unsigned long long)addr, min_size);
    region = hb_memory_find_region(ctx->memory, addr);
    if (!region || !(region->perm & HB_PERM_READ)) return NULL;
    region_end = region->base + region->size;
    if (region_end < region->base || addr < region->base || addr >= region_end) return NULL;
    if (min_size > (size_t)(region_end - addr)) return NULL;
    if (region->host_base) {
        const uint8_t* hp = (const uint8_t*)region->host_base + (addr - region->base);
        if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-host_read_span: -> region host=%p (host_base=%p base=0x%llx)\n", (void*)hp, (void*)region->host_base, (unsigned long long)region->base);
        if (available) *available = (size_t)(region_end - addr);
        return hp;
    }
    if (macrunner_hb_codegendv_hit(addr)) fprintf(stderr, "macrunner-hb-dv-host_read_span: -> identity host=%p\n", (void*)(uintptr_t)addr);
    if (available) *available = (size_t)(region_end - addr);
    return (const uint8_t*)(uintptr_t)addr;
}

static hb_result_t hb_jit_helper_read_u8_fast(hb_context_t* ctx, uint64_t addr, uint8_t* out) {
    size_t available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, addr, sizeof(*out), &available);
    (void)available;
    if (span) {
        *out = hb_jit_helper_host_load_u8_acquire(span);
        return HB_OK;
    }
    return hb_jit_helper_read_u8_tso(ctx, addr, out);
}

static hb_result_t hb_jit_helper_read_u16_fast(hb_context_t* ctx, uint64_t addr, uint16_t* out) {
    size_t available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, addr, sizeof(*out), &available);
    (void)available;
    if (span) {
        *out = hb_jit_helper_host_load_u16_acquire(span);
        return HB_OK;
    }
    return hb_jit_helper_read_u16_tso(ctx, addr, out);
}

static hb_result_t hb_jit_helper_read_u32_fast(hb_context_t* ctx, uint64_t addr, uint32_t* out) {
    size_t available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, addr, sizeof(*out), &available);
    (void)available;
    if (span) {
        *out = hb_jit_helper_host_load_u32_acquire(span);
        return HB_OK;
    }
    return hb_jit_helper_read_u32_tso(ctx, addr, out);
}

static hb_result_t hb_jit_helper_read_u64_fast(hb_context_t* ctx, uint64_t addr, uint64_t* out) {
    size_t available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, addr, sizeof(*out), &available);
    (void)available;
    if (span) {
        *out = hb_jit_helper_host_load_u64_acquire(span);
        return HB_OK;
    }
    return hb_jit_helper_read_u64_tso(ctx, addr, out);
}

static hb_result_t hb_jit_helper_read_mono_metadata_node_fast(hb_context_t* ctx,
                                                              uint64_t node,
                                                              uint64_t* out_base,
                                                              uint32_t* out_delta) {
    size_t available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, node + 16u, 16u, &available);
    (void)available;
    if (span) {
        *out_base = hb_jit_helper_host_load_u64_acquire(span);
        *out_delta = hb_jit_helper_host_load_u32_acquire(span + 12u);
        return HB_OK;
    }

    hb_result_t r = hb_jit_helper_read_u32_fast(ctx, node + 28u, out_delta);
    if (r != HB_OK) return r;
    return hb_jit_helper_read_u64_fast(ctx, node + 16u, out_base);
}

void hb_jit_helper_exec_mono_string_hash(hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    uint64_t rsp, ret_addr, rcx;
    uint32_t eax = 0;
    uint8_t ch = 0;
    bool loop_entered = false;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_string_hash_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rsp = ctx->regs.x64.rsp;
    rcx = ctx->regs.x64.rcx;
    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }
    size_t span_available = 0;
    const uint8_t* span = hb_jit_helper_host_read_span(ctx, rcx, 1, &span_available);
    if (span) {
        ch = hb_jit_helper_host_load_u8_acquire(span);
    } else {
        r = hb_jit_helper_read_u8_fast(ctx, rcx, &ch);
        if (r != HB_OK) { ctx->last_result = r; return; }
    }

    if (ch != 0) {
        loop_entered = true;
        uint64_t base = rcx;
        for (uint64_t iter = 0;; iter++) {
            uint64_t pos = iter + 1u;
            if (iter >= (1u << 20)) {
                ctx->last_result = HB_ERR_EXEC_FAULT;
                return;
            }
            if (span && pos < span_available) {
                ch = hb_jit_helper_host_load_u8_acquire(span + pos);
            } else {
                r = hb_jit_helper_read_u8_fast(ctx, base + pos, &ch);
                if (r != HB_OK) { ctx->last_result = r; return; }
            }
            rcx = base + pos;
            eax = (uint32_t)(eax * 31u - (uint32_t)(int32_t)(int8_t)ch);
            if (ch == 0) break;
        }
    }

    ctx->regs.x64.rax = eax;
    ctx->regs.x64.rcx = rcx;
    if (loop_entered) {
        ctx->regs.x64.r8 = 0;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, HB_SIZE_8, 0, 0, 0, 0);
    } else {
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_8, 0, 0, 0, 0);
    }
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_mono_string_equal(hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    uint64_t rsp, ret_addr, rcx, rdx, offset;
    uint32_t eax = 0;
    uint32_t r8d = 0;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_string_equal_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rsp = ctx->regs.x64.rsp;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }

    if (rcx == rdx) {
        ctx->regs.x64.rax = 1;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64, rcx, rdx, 0, 0);
        ctx->regs.x64.rsp = rsp + 8u;
        ctx->pc = ret_addr;
        hb_jit_helper_sync_pc(ctx);
        ctx->last_result = HB_OK;
        return;
    }

    offset = rdx - rcx;
    rdx = offset;
    size_t lhs_available = 0;
    size_t rhs_available = 0;
    const uint8_t* lhs_span = hb_jit_helper_host_read_span(ctx, rcx, 1, &lhs_available);
    const uint8_t* rhs_span = hb_jit_helper_host_read_span(ctx, rcx + offset, 1, &rhs_available);
    for (uint64_t iter = 0;; iter++) {
        uint8_t lhs = 0;
        uint8_t rhs = 0;
        if (iter >= (1u << 20)) {
            ctx->last_result = HB_ERR_EXEC_FAULT;
            return;
        }
        if (lhs_span && iter < lhs_available) {
            lhs = hb_jit_helper_host_load_u8_acquire(lhs_span + iter);
        } else {
            r = hb_jit_helper_read_u8_fast(ctx, rcx, &lhs);
            if (r != HB_OK) { ctx->last_result = r; return; }
        }
        if (rhs_span && iter < rhs_available) {
            rhs = hb_jit_helper_host_load_u8_acquire(rhs_span + iter);
        } else {
            r = hb_jit_helper_read_u8_fast(ctx, rcx + offset, &rhs);
            if (r != HB_OK) { ctx->last_result = r; return; }
        }
        eax = rhs;
        r8d = (uint32_t)lhs - (uint32_t)rhs;
        if (r8d != 0) {
            eax = 0;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_XOR, HB_SIZE_32, 0, 0, 0, 0);
            break;
        }
        rcx++;
        if (rhs == 0) {
            eax = 1;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, HB_SIZE_32, 0, 0, 0, 0);
            break;
        }
    }

    ctx->regs.x64.rax = eax;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.r8 = r8d;
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_mono_metadata_rowptr_entry(hb_context_t* ctx,
                                                   const hb_ir_block_t* block) {
    uint64_t rsp, ret_addr, table, base;
    uint32_t idx, rows;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_rowptr_entry_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rsp = ctx->regs.x64.rsp;
    table = ctx->regs.x64.rcx;
    idx = (uint32_t)ctx->regs.x64.rdx;
    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, table + 0x80u, &rows);
    if (r != HB_OK) { ctx->last_result = r; return; }

    if (idx >= rows) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    r = hb_jit_helper_read_u64_fast(ctx, table + 0x78u, &base);
    if (r != HB_OK) { ctx->last_result = r; return; }

    ctx->regs.x64.rax = base + idx;
    ctx->regs.x64.rcx = rows;
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, HB_SIZE_64,
                       rsp - 0x48u, 0x40u, rsp - 8u, 0);
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_mono_metadata_bsearch_loop(hb_context_t* ctx,
                                                   const hb_ir_block_t* block) {
    const hb_ir_instr_t* i;
    size_t n = 0;
    uint64_t rax, rbx, rcx, rdx, rbp, r8, r9, r10;
    uint64_t loop_pc;
    uint64_t fallthrough_pc;
    size_t table_available = 0;
    const uint8_t* table_span;
    hb_result_t r = HB_OK;
    bool loop_taken = false;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_bsearch_loop_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    i = mono_metadata_bsearch_loop_instrs(block, &n);
    loop_pc = i[0].guest_addr;
    fallthrough_pc = i[17].guest_addr + i[17].guest_len;

    rax = ctx->regs.x64.rax;
    rbx = ctx->regs.x64.rbx;
    rcx = ctx->regs.x64.rcx;
    rdx = ctx->regs.x64.rdx;
    rbp = ctx->regs.x64.rbp;
    r8 = ctx->regs.x64.r8;
    r9 = ctx->regs.x64.r9;
    r10 = ctx->regs.x64.r10;
    table_span = hb_jit_helper_host_read_span(ctx, rbx + 24u, sizeof(uint64_t), &table_available);

    for (uint64_t iter = 0; iter < (1u << 20); iter++) {
        uint32_t eax = (uint32_t)r9 + (uint32_t)rbp;
        uint32_t ecx;
        uint32_t tmp32 = 0;
        uint64_t node = 0;
        bool cmp_above_or_equal;

        rax = eax;
        rdx = ((int32_t)eax < 0) ? 0xffffffffu : 0u;
        eax = (uint32_t)(eax - (uint32_t)rdx);
        eax = (uint32_t)((int32_t)eax >> 1);
        rax = eax;
        rcx = (uint64_t)(int64_t)(int32_t)eax;

        if (table_span && rcx <= ((uint64_t)(~(size_t)0) / 8u)) {
            size_t table_offset = (size_t)(rcx * 8u);
            if (table_available >= sizeof(node) &&
                table_offset <= table_available - sizeof(node)) {
                node = hb_jit_helper_host_load_u64_acquire(table_span + table_offset);
            } else {
                r = hb_jit_helper_read_u64_fast(ctx, rbx + rcx * 8u + 24u, &node);
                if (r != HB_OK) break;
            }
        } else {
            r = hb_jit_helper_read_u64_fast(ctx, rbx + rcx * 8u + 24u, &node);
            if (r != HB_OK) break;
        }
        rcx = node;
        r = hb_jit_helper_read_mono_metadata_node_fast(ctx, rcx, &rcx, &tmp32);
        if (r != HB_OK) break;
        r8 = (uint64_t)(int64_t)(int32_t)tmp32;
        r8 += rcx;
        ecx = eax;

        cmp_above_or_equal = r10 >= r8;
        eax = (uint32_t)(eax + 1u);
        rax = eax;
        if (cmp_above_or_equal) ecx = (uint32_t)r9;
        r9 = ecx;
        if (cmp_above_or_equal) rbp = eax;
        rcx = ecx;
        loop_taken = (int32_t)(uint32_t)rbp < (int32_t)ecx;
        if (!loop_taken) break;
    }

    ctx->regs.x64.rax = rax;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.rbp = rbp;
    ctx->regs.x64.r8 = r8;
    ctx->regs.x64.r9 = r9;

    if (r == HB_OK) {
        uint32_t lhs = (uint32_t)rbp;
        uint32_t rhs = (uint32_t)rcx;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_32, lhs, rhs,
                           (uint32_t)(lhs - rhs), 0);
        ctx->pc = loop_taken ? loop_pc : fallthrough_pc;
        hb_jit_helper_sync_pc(ctx);
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_mono_metadata_decode_row_loop(hb_context_t* ctx,
                                                      const hb_ir_block_t* block) {
    uint64_t rax, rbx, rcx, rdx, r8, r9, r14, r15;
    uint64_t fallthrough_pc;
    hb_result_t r = HB_OK;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_decode_row_loop_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rax = ctx->regs.x64.rax;
    rbx = ctx->regs.x64.rbx;
    rcx = (uint32_t)ctx->regs.x64.rdx;
    rdx = ctx->regs.x64.rdx;
    r8 = ctx->regs.x64.r8;
    r9 = ctx->regs.x64.r9;
    r14 = ctx->regs.x64.r14;
    r15 = ctx->regs.x64.r15;
    fallthrough_pc = block->guest_addr + 0x4c;

    for (uint64_t iter = 0; (int64_t)rdx < (int64_t)r9; iter++) {
        uint32_t width = ((uint32_t)r15 >> ((uint8_t)rcx & 31)) & 3u;
        uint32_t value = 0;
        uint8_t v8 = 0;
        uint16_t v16 = 0;

        if (iter >= (1u << 20)) {
            r = HB_ERR_EXEC_FAULT;
            break;
        }

        width++;
        r8 = width;
        rax = width - 1u;
        if ((uint32_t)rax == 0) {
            r = hb_jit_helper_read_u8_fast(ctx, rbx, &v8);
            if (r != HB_OK) break;
            value = (uint32_t)(int32_t)(int8_t)v8;
        } else {
            rax = (uint32_t)rax - 1u;
            if ((uint32_t)rax == 0) {
                r = hb_jit_helper_read_u16_fast(ctx, rbx, &v16);
                if (r != HB_OK) break;
                value = v16;
            } else if ((uint32_t)rax == 2) {
                r = hb_jit_helper_read_u32_fast(ctx, rbx, &value);
                if (r != HB_OK) break;
            } else {
                ctx->regs.x64.rax = rax;
                ctx->regs.x64.rcx = rcx;
                ctx->regs.x64.rdx = rdx;
                ctx->regs.x64.r8 = r8;
                hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_32,
                                   (uint32_t)rax, 2u, (uint32_t)rax - 2u, 0);
                ctx->pc = block->guest_addr + 0x65;
                hb_jit_helper_sync_pc(ctx);
                ctx->last_result = HB_OK;
                return;
            }
        }

        r = hb_jit_helper_write_u32_tso(ctx, r14 + rdx * 4u, value);
        if (r != HB_OK) break;
        rcx = (uint32_t)((uint32_t)rcx + 2u);
        rax = width;
        rdx++;
        rbx += rax;
    }

    ctx->regs.x64.rax = rax;
    ctx->regs.x64.rbx = rbx;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.r8 = r8;

    if (r == HB_OK) {
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64,
                           rdx, r9, rdx - r9, 0);
        ctx->pc = fallthrough_pc;
        hb_jit_helper_sync_pc(ctx);
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_mono_metadata_decode_row_entry(hb_context_t* ctx,
                                                       const hb_ir_block_t* block) {
    uint64_t table, out_ptr, rsp, ret_addr, base, row, rax, rcx, rdx, r8, r9;
    uint32_t rows_field, rows, bits, cols, idx, res_size, last_width;
    uint32_t values[64];
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_decode_row_entry_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    table = ctx->regs.x64.rcx;
    idx = (uint32_t)ctx->regs.x64.rdx;
    out_ptr = ctx->regs.x64.r8;
    cols = (uint32_t)ctx->regs.x64.r9;
    rsp = ctx->regs.x64.rsp;

    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u64_fast(ctx, table, &base);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, table + 8u, &rows_field);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, table + 12u, &bits);
    if (r != HB_OK) { ctx->last_result = r; return; }

    rows = rows_field & 0xffffffu;
    res_size = bits >> 24;
    if (idx >= rows || (int32_t)idx < 0 || cols != res_size ||
        cols > (uint32_t)(sizeof(values) / sizeof(values[0])) ||
        (!out_ptr && cols)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    row = base + (uint32_t)(idx * (uint8_t)(rows_field >> 24));
    last_width = (uint8_t)(rows_field >> 24);
    for (uint32_t col = 0, shift = 0; col < cols; col++, shift += 2) {
        uint32_t width = ((bits >> (shift & 31u)) & 3u) + 1u;
        if (width == 1u) {
            uint8_t v = 0;
            r = hb_jit_helper_read_u8_fast(ctx, row, &v);
            if (r != HB_OK) { ctx->last_result = r; return; }
            values[col] = (uint32_t)(int32_t)(int8_t)v;
        } else if (width == 2u) {
            uint16_t v = 0;
            r = hb_jit_helper_read_u16_fast(ctx, row, &v);
            if (r != HB_OK) { ctx->last_result = r; return; }
            values[col] = v;
        } else if (width == 4u) {
            r = hb_jit_helper_read_u32_fast(ctx, row, &values[col]);
            if (r != HB_OK) { ctx->last_result = r; return; }
        } else {
            ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
            return;
        }
        last_width = width;
        row += width;
    }

    for (uint32_t col = 0; col < cols; col++) {
        r = hb_jit_helper_write_u32_tso(ctx, out_ptr + (uint64_t)col * 4u, values[col]);
        if (r != HB_OK) { ctx->last_result = r; return; }
    }

    rdx = cols;
    r9 = cols;
    if (cols) {
        rax = last_width;
        rcx = (uint32_t)(cols * 2u);
        r8 = last_width;
    } else {
        rax = last_width;
        rcx = table;
        r8 = out_ptr;
    }

    ctx->regs.x64.rax = rax;
    ctx->regs.x64.rcx = rcx;
    ctx->regs.x64.rdx = rdx;
    ctx->regs.x64.r8 = r8;
    ctx->regs.x64.r9 = r9;
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, HB_SIZE_64, rdx, r9, rdx - r9, 0);
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_mono_metadata_decode_col(hb_context_t* ctx,
                                                 const hb_ir_block_t* block) {
    uint64_t table, rsp, ret_addr, base, row;
    uint32_t rows_field, rows, bits, idx, col, res_size, eax, edi;
    uint8_t row_size;
    uint64_t r8, r9, r10;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_decode_col_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    table = ctx->regs.x64.rcx;
    idx = (uint32_t)ctx->regs.x64.rdx;
    col = (uint32_t)ctx->regs.x64.r8;
    rsp = ctx->regs.x64.rsp;

    r = hb_jit_helper_read_u32_fast(ctx, table + 8u, &rows_field);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_fast(ctx, table + 12u, &bits);
    if (r != HB_OK) { ctx->last_result = r; return; }
    rows = rows_field & 0xffffffu;
    res_size = bits >> 24;
    if (idx >= rows || col >= res_size) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }
    r = hb_jit_helper_read_u8_fast(ctx, table + 11u, &row_size);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u64_fast(ctx, table, &base);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }

    r8 = base + (uint32_t)((uint32_t)row_size * idx);
    r9 = 0;
    r10 = 0;
    eax = (bits & 3u) + 1u;
    edi = 0;

    if (col >= 2u) {
        uint32_t ecx = ((col - 2u) >> 1) + 1u;
        uint32_t edx = 4u;
        uint64_t r11 = ecx;
        edi = ecx + ecx;
        do {
            ecx = eax;
            eax = bits;
            r9 += ecx;
            ecx = edx - 2u;
            eax >>= (ecx & 31u);
            ecx = edx;
            eax &= 3u;
            edx += 4u;
            eax++;
            r10 += eax;
            eax = bits;
            eax >>= (ecx & 31u);
            eax &= 3u;
            eax++;
            r11--;
        } while (r11 != 0);
    }

    if (edi < col) {
        uint32_t ecx = eax;
        eax = bits;
        r8 += ecx;
        ecx = edi * 2u + 2u;
        eax >>= (ecx & 31u);
        eax &= 3u;
        eax++;
    }

    row = r8 + r10 + r9;
    eax--;
    if (eax == 0) {
        uint8_t v = 0;
        r = hb_jit_helper_read_u8_fast(ctx, row, &v);
        if (r != HB_OK) { ctx->last_result = r; return; }
        ctx->regs.x64.rax = (uint32_t)(int32_t)(int8_t)v;
    } else {
        eax--;
        if (eax == 0) {
            uint16_t v = 0;
            r = hb_jit_helper_read_u16_fast(ctx, row, &v);
            if (r != HB_OK) { ctx->last_result = r; return; }
            ctx->regs.x64.rax = v;
        } else if (eax == 2) {
            uint32_t v = 0;
            r = hb_jit_helper_read_u32_fast(ctx, row, &v);
            if (r != HB_OK) { ctx->last_result = r; return; }
            ctx->regs.x64.rax = v;
        } else {
            ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
            return;
        }
    }

    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, HB_SIZE_64,
                       rsp - 0x28u, 0x20u, rsp - 8u, 0);
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

static hb_result_t mono_metadata_decode_col_value(hb_context_t* ctx, uint64_t table,
                                                  uint32_t idx, uint32_t col,
                                                  uint32_t* out_value) {
    uint32_t rows_field, rows, bits, res_size, eax, edi;
    uint8_t row_size;
    uint64_t base, r8, r9, r10, row;
    hb_result_t r;

    if (!ctx || !ctx->memory || !out_value) return HB_ERR_INVALID_ARG;
    r = hb_jit_helper_read_u32_fast(ctx, table + 8u, &rows_field);
    if (r != HB_OK) return r;
    r = hb_jit_helper_read_u32_fast(ctx, table + 12u, &bits);
    if (r != HB_OK) return r;
    rows = rows_field & 0xffffffu;
    res_size = bits >> 24;
    if (idx >= rows || col >= res_size) return HB_ERR_INVALID_ARG;
    r = hb_jit_helper_read_u8_fast(ctx, table + 11u, &row_size);
    if (r != HB_OK) return r;
    r = hb_jit_helper_read_u64_fast(ctx, table, &base);
    if (r != HB_OK) return r;

    r8 = base + (uint32_t)((uint32_t)row_size * idx);
    r9 = 0;
    r10 = 0;
    eax = (bits & 3u) + 1u;
    edi = 0;

    if (col >= 2u) {
        uint32_t ecx = ((col - 2u) >> 1) + 1u;
        uint32_t edx = 4u;
        uint64_t r11 = ecx;
        edi = ecx + ecx;
        do {
            ecx = eax;
            eax = bits;
            r9 += ecx;
            ecx = edx - 2u;
            eax >>= (ecx & 31u);
            ecx = edx;
            eax &= 3u;
            edx += 4u;
            eax++;
            r10 += eax;
            eax = bits;
            eax >>= (ecx & 31u);
            eax &= 3u;
            eax++;
            r11--;
        } while (r11 != 0);
    }

    if (edi < col) {
        uint32_t ecx = eax;
        eax = bits;
        r8 += ecx;
        ecx = edi * 2u + 2u;
        eax >>= (ecx & 31u);
        eax &= 3u;
        eax++;
    }

    row = r8 + r10 + r9;
    eax--;
    if (eax == 0) {
        uint8_t v = 0;
        r = hb_jit_helper_read_u8_fast(ctx, row, &v);
        if (r != HB_OK) return r;
        *out_value = (uint32_t)(int32_t)(int8_t)v;
    } else {
        eax--;
        if (eax == 0) {
            uint16_t v = 0;
            r = hb_jit_helper_read_u16_fast(ctx, row, &v);
            if (r != HB_OK) return r;
            *out_value = v;
        } else if (eax == 2) {
            r = hb_jit_helper_read_u32_fast(ctx, row, out_value);
            if (r != HB_OK) return r;
        } else {
            return HB_ERR_INVALID_ARG;
        }
    }
    return HB_OK;
}

void hb_jit_helper_exec_mono_metadata_coded_index_search(hb_context_t* ctx,
                                                         const hb_ir_block_t* block) {
    uint64_t rsp, ret_addr, image, table, out_ptr, table_base;
    uint32_t token, token_type, tag, key, rows_field, rows, value;
    uint8_t row_size;
    hb_result_t r;

    if (!ctx || !block) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    if (ctx->mode != HB_MODE_64BIT || !mono_metadata_coded_index_search_candidate(block)) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    rsp = ctx->regs.x64.rsp;
    image = ctx->regs.x64.rcx;
    token = (uint32_t)ctx->regs.x64.rdx;
    out_ptr = ctx->regs.x64.r8;
    table = image + 0x390u;

    if (!out_ptr) {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    r = hb_jit_read_guest_u64_result(ctx, rsp, &ret_addr);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u64_fast(ctx, table, &table_base);
    if (r != HB_OK) { ctx->last_result = r; return; }

    if (!table_base) {
        ctx->regs.x64.rax = 0;
        hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, HB_SIZE_64,
                           rsp - 0x48u, 0x30u, rsp - 0x18u, 0);
        ctx->regs.x64.rsp = rsp + 8u;
        ctx->pc = ret_addr;
        hb_jit_helper_sync_pc(ctx);
        ctx->last_result = HB_OK;
        return;
    }

    token_type = token >> 24;
    if (token_type == 2u) {
        tag = 0;
    } else if (token_type == 6u) {
        tag = 1;
    } else {
        ctx->last_result = hb_jit_helper_exec_ir_block_once(ctx, block);
        return;
    }

    key = ((token & 0xffffffu) << 1) | tag;
    r = hb_jit_helper_write_u32_tso(ctx, out_ptr, key);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u32_tso(ctx, table + 8u, &rows_field);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_jit_helper_read_u8_tso(ctx, table + 11u, &row_size);
    if (r != HB_OK) { ctx->last_result = r; return; }
    rows = rows_field & 0xffffffu;

    ctx->regs.x64.rax = 0;
    if (rows && row_size) {
        uint64_t low = table_base;
        uint64_t count = rows;
        uint32_t found = UINT32_MAX;

        for (uint64_t iter = 0; count > 0; iter++) {
            uint64_t mid_ptr;
            uint32_t mid;
            if (iter >= (1u << 20)) {
                ctx->last_result = HB_ERR_EXEC_FAULT;
                return;
            }
            mid_ptr = low + (count >> 1) * (uint64_t)row_size;
            mid = (uint32_t)((mid_ptr - table_base) / row_size);
            r = mono_metadata_decode_col_value(ctx, table, mid, 2u, &value);
            if (r != HB_OK) { ctx->last_result = r; return; }
            if (key == value) {
                found = mid;
                break;
            }
            if (key < value) {
                count >>= 1;
            } else {
                low = mid_ptr + row_size;
                count = (count - 1u) >> 1;
            }
        }

        if (found != UINT32_MAX) {
            while (found > 0) {
                r = mono_metadata_decode_col_value(ctx, table, found - 1u, 2u, &value);
                if (r != HB_OK) { ctx->last_result = r; return; }
                if (value != key) break;
                found--;
            }
            ctx->regs.x64.rax = (uint64_t)found + 1u;
        }
    }

    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, HB_SIZE_64,
                       rsp - 0x48u, 0x30u, rsp - 0x18u, 0);
    ctx->regs.x64.rsp = rsp + 8u;
    ctx->pc = ret_addr;
    hb_jit_helper_sync_pc(ctx);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_setcc_lazy(hb_context_t* ctx, uint64_t cc, uint64_t dst_reg) {
    bool value = false;
    hb_result_t r = hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value);
    if (ctx) ctx->last_result = r;
    if (r != HB_OK) return;
    hb_context_write_reg_value_sized(ctx, dst_reg, value ? 1 : 0, HB_SIZE_8);
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_cmovcc_lazy(hb_context_t* ctx, uint64_t cc, uint64_t dst_reg,
                                    uint64_t src_is_reg, uint64_t src_value, uint64_t size) {
    bool value = false;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK || !value) return;
    uint64_t src = src_is_reg ? hb_context_read_reg_value(ctx, src_value) : src_value;
    hb_context_write_reg_value_sized(ctx, dst_reg, src, (hb_size_t)size);
}

void hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    hb_jit_helper_release_before_operand_write(&instr->dst);
    (void)hb_flags_write_operand_value(ctx, &instr->dst, src);
}

void hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) return;
    if (hb_jit_operand_is_guest_mem(&instr->dst) ||
        hb_jit_operand_is_guest_mem(&instr->src1) ||
        hb_jit_operand_is_guest_mem(&instr->src2))
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)hb_flags_exec_binop_operand(ctx, instr->op, &instr->dst, &instr->src1, &instr->src2, NULL);
    if (hb_jit_operand_is_guest_mem(&instr->dst) ||
        hb_jit_operand_is_guest_mem(&instr->src1) ||
        hb_jit_operand_is_guest_mem(&instr->src2))
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) return;
    if (hb_jit_operand_is_guest_mem(&instr->dst) ||
        hb_jit_operand_is_guest_mem(&instr->src1) ||
        hb_jit_operand_is_guest_mem(&instr->src2))
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)hb_flags_exec_double_shift_operand(ctx, instr->op, &instr->dst, &instr->src1, &instr->src2, NULL);
    if (hb_jit_operand_is_guest_mem(&instr->dst) ||
        hb_jit_operand_is_guest_mem(&instr->src1) ||
        hb_jit_operand_is_guest_mem(&instr->src2))
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr) {
    bool value = false;
    if (!ctx || !instr) return;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK) return;
    hb_jit_helper_release_before_operand_write(&instr->dst);
    (void)hb_flags_write_operand_value(ctx, &instr->dst, value ? 1 : 0);
}

void hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr) {
    bool value = false;
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK || !value) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    hb_jit_helper_release_before_operand_write(&instr->dst);
    (void)hb_flags_write_operand_value(ctx, &instr->dst, src);
}

void hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    hb_jit_helper_release_before_operand_write(&instr->dst);
    (void)hb_flags_write_operand_value(ctx, &instr->dst, ~src);
}

void hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
    if (!size) size = HB_SIZE_64;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    uint64_t mask = (size == HB_SIZE_8) ? 0xffULL :
                    (size == HB_SIZE_16) ? 0xffffULL :
                    (size == HB_SIZE_32) ? 0xffffffffULL : ~0ULL;
    uint64_t lhs = 0;
    uint64_t rhs = src & mask;
    uint64_t result = (0 - rhs) & mask;
    hb_jit_helper_release_before_operand_write(&instr->dst);
    if (hb_flags_write_operand_value(ctx, &instr->dst, result) != HB_OK) return;
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_SUB, size, lhs, rhs, result, 0);
}

void hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    uint64_t result = 0;
    uint64_t width;
    hb_size_t size;
    hb_result_t r;

    if (!ctx || !instr || instr->dst.type != HB_OP_REG) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &val);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    hb_jit_helper_acquire_after_operand_read(&instr->src1);
    size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
    val = hb_jit_trunc_to_size(val, size);
    width = (size == HB_SIZE_32) ? 32 : (size == HB_SIZE_16) ? 16 :
            (size == HB_SIZE_8) ? 8 : 64;

    hb_lazy_flags_clear(ctx);
    if (instr->op == HB_IR_BSF) {
        ctx->flags.zf = (val == 0);
        if (val) {
            while (((val >> result) & 1ULL) == 0) result++;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        }
    } else if (instr->op == HB_IR_TZCNT) {
        result = width;
        if (val) {
            result = 0;
            while (((val >> result) & 1ULL) == 0) result++;
        }
        hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        ctx->flags.zf = (result == 0);
        ctx->flags.cf = (val == 0);
    } else if (instr->op == HB_IR_LZCNT) {
        result = width;
        if (val) {
            result = 0;
            for (int64_t bit = (int64_t)width - 1; bit >= 0 && ((val >> bit) & 1ULL) == 0; bit--)
                result++;
        }
        hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        ctx->flags.zf = (result == 0);
        ctx->flags.cf = (val == 0);
    } else if (instr->op == HB_IR_BSR) {
        ctx->flags.zf = (val == 0);
        if (val) {
            result = width - 1;
            while (((val >> result) & 1ULL) == 0) result--;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        }
    }
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr || instr->dst.type != HB_OP_REG) return;

    uint64_t value = 0;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &value) != HB_OK) return;
    hb_jit_helper_acquire_after_operand_read(&instr->src1);

    switch (instr->src1.size) {
        case HB_SIZE_8:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int8_t)value
                                                     : (value & 0xffULL);
            break;
        case HB_SIZE_16:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int16_t)value
                                                     : (value & 0xffffULL);
            break;
        case HB_SIZE_32:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int32_t)value
                                                     : (value & 0xffffffffULL);
            break;
        case HB_SIZE_64:
        default:
            break;
    }

    hb_context_write_reg_value_sized(ctx, (uint64_t)instr->dst.reg, value, instr->dst.size);
}

void hb_jit_helper_lahf(hb_context_t* ctx) {
    if (!ctx) return;
    if (hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_SF | HB_FLAG_BIT_ZF |
                                      HB_FLAG_BIT_AF | HB_FLAG_BIT_PF |
                                      HB_FLAG_BIT_CF) != HB_OK) return;
    uint8_t ah = 0x02;
    if (ctx->flags.sf) ah |= 0x80;
    if (ctx->flags.zf) ah |= 0x40;
    if (ctx->flags.af) ah |= 0x10;
    if (ctx->flags.pf) ah |= 0x04;
    if (ctx->flags.cf) ah |= 0x01;
    if (ctx->mode == HB_MODE_32BIT) {
        ctx->regs.x86.eax = (ctx->regs.x86.eax & ~0x0000ff00U) | ((uint32_t)ah << 8);
    } else {
        ctx->regs.x64.rax = (ctx->regs.x64.rax & ~0x000000000000ff00ULL) | ((uint64_t)ah << 8);
    }
}

void hb_jit_helper_sahf(hb_context_t* ctx) {
    if (!ctx) return;
    uint8_t ah = ctx->mode == HB_MODE_32BIT ?
        (uint8_t)((ctx->regs.x86.eax >> 8) & 0xffU) :
        (uint8_t)((ctx->regs.x64.rax >> 8) & 0xffU);
    hb_lazy_flags_clear(ctx);
    ctx->flags.sf = (ah & 0x80) != 0;
    ctx->flags.zf = (ah & 0x40) != 0;
    ctx->flags.af = (ah & 0x10) != 0;
    ctx->flags.pf = (ah & 0x04) != 0;
    ctx->flags.cf = (ah & 0x01) != 0;
}

static uint64_t jit_trunc_val(uint64_t v, uint64_t sz) {
    switch (sz) {
        case 1: return v & 0xFFULL;
        case 2: return v & 0xFFFFULL;
        case 4: return v & 0xFFFFFFFFULL;
        default: return v;
    }
}

static unsigned jit_msb_bit(uint64_t sz) {
    switch (sz) {
        case 1: return 7;
        case 2: return 15;
        case 4: return 31;
        default: return 63;
    }
}

static uint64_t jit_read_reg(hb_context_t* ctx, uint64_t idx) {
    if (!ctx) return 0;
    if (ctx->mode == HB_MODE_32BIT) {
        switch (idx) {
            case 0: return ctx->regs.x86.eax;
            case 1: return ctx->regs.x86.ecx;
            case 2: return ctx->regs.x86.edx;
            case 3: return ctx->regs.x86.ebx;
            case 4: return ctx->regs.x86.esp;
            case 5: return ctx->regs.x86.ebp;
            case 6: return ctx->regs.x86.esi;
            case 7: return ctx->regs.x86.edi;
            default: return 0;
        }
    }
    switch (idx) {
        case 0: return ctx->regs.x64.rax;
        case 1: return ctx->regs.x64.rcx;
        case 2: return ctx->regs.x64.rdx;
        case 3: return ctx->regs.x64.rbx;
        case 4: return ctx->regs.x64.rsp;
        case 5: return ctx->regs.x64.rbp;
        case 6: return ctx->regs.x64.rsi;
        case 7: return ctx->regs.x64.rdi;
        case 8: return ctx->regs.x64.r8;
        case 9: return ctx->regs.x64.r9;
        case 10: return ctx->regs.x64.r10;
        case 11: return ctx->regs.x64.r11;
        case 12: return ctx->regs.x64.r12;
        case 13: return ctx->regs.x64.r13;
        case 14: return ctx->regs.x64.r14;
        case 15: return ctx->regs.x64.r15;
        default: return 0;
    }
}

static void jit_write_reg(hb_context_t* ctx, uint64_t idx, uint64_t val) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT) {
        switch (idx) {
            case 0: ctx->regs.x86.eax = (uint32_t)val; return;
            case 1: ctx->regs.x86.ecx = (uint32_t)val; return;
            case 2: ctx->regs.x86.edx = (uint32_t)val; return;
            case 3: ctx->regs.x86.ebx = (uint32_t)val; return;
            case 4: ctx->regs.x86.esp = (uint32_t)val; return;
            case 5: ctx->regs.x86.ebp = (uint32_t)val; return;
            case 6: ctx->regs.x86.esi = (uint32_t)val; return;
            case 7: ctx->regs.x86.edi = (uint32_t)val; return;
            default: return;
        }
    }
    switch (idx) {
        case 0: ctx->regs.x64.rax = val; return;
        case 1: ctx->regs.x64.rcx = val; return;
        case 2: ctx->regs.x64.rdx = val; return;
        case 3: ctx->regs.x64.rbx = val; return;
        case 4: ctx->regs.x64.rsp = val; return;
        case 5: ctx->regs.x64.rbp = val; return;
        case 6: ctx->regs.x64.rsi = val; return;
        case 7: ctx->regs.x64.rdi = val; return;
        case 8: ctx->regs.x64.r8 = val; return;
        case 9: ctx->regs.x64.r9 = val; return;
        case 10: ctx->regs.x64.r10 = val; return;
        case 11: ctx->regs.x64.r11 = val; return;
        case 12: ctx->regs.x64.r12 = val; return;
        case 13: ctx->regs.x64.r13 = val; return;
        case 14: ctx->regs.x64.r14 = val; return;
        case 15: ctx->regs.x64.r15 = val; return;
        default: return;
    }
}

void hb_jit_helper_cpuid(hb_context_t* ctx) {
    if (!ctx) return;

    uint32_t leaf = (uint32_t)jit_read_reg(ctx, HB_REG_RAX);
    uint32_t subleaf = (uint32_t)jit_read_reg(ctx, HB_REG_RCX);
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    (void)subleaf;

    switch (leaf) {
        case 0:
            eax = 7;
            ebx = 0x756e6547U; /* "Genu" */
            edx = 0x49656e69U; /* "ineI" */
            ecx = 0x6c65746eU; /* "ntel" */
            break;
        case 1:
            eax = 0x000306a9U;
            /* Conservative feature surface: do not advertise AVX/OSXSAVE or
             * other optional opcode families until the bridge implements them.
             */
            ecx = 0;
            edx = (1U << 8)  |  /* CX8 */
                  (1U << 15) |  /* CMOV */
                  (1U << 23) |  /* MMX */
                  (1U << 24) |  /* FXSR */
                  (1U << 25) |  /* SSE */
                  (1U << 26);   /* SSE2 */
            break;
        default:
            break;
    }

    jit_write_reg(ctx, HB_REG_RAX, eax);
    jit_write_reg(ctx, HB_REG_RBX, ebx);
    jit_write_reg(ctx, HB_REG_RCX, ecx);
    jit_write_reg(ctx, HB_REG_RDX, edx);
}

void hb_jit_helper_xgetbv(hb_context_t* ctx) {
    if (!ctx) return;

    uint32_t ecx = (uint32_t)jit_read_reg(ctx, HB_REG_RCX);
    uint64_t xcr0 = ecx == 0 ? 0x3ULL : 0;

    jit_write_reg(ctx, HB_REG_RAX, (uint32_t)xcr0);
    jit_write_reg(ctx, HB_REG_RDX, (uint32_t)(xcr0 >> 32));
}

uint64_t hb_jit_helper_exec_shift(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                  uint64_t src1_reg, uint64_t src2_type,
                                  uint64_t src2_value, uint64_t size) {
    uint64_t a = jit_read_reg(ctx, src1_reg);
    uint64_t shift = src2_type == 1 ? jit_read_reg(ctx, src2_value) : src2_value;
    shift &= (size == 8) ? 0x3FULL : 0x1FULL;
    if (shift == 0) {
        jit_write_reg(ctx, dst_reg, a);
        return a;
    }

    unsigned msb = jit_msb_bit(size);
    uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << (msb + 1)) - 1ULL);
    uint64_t ta = jit_trunc_val(a, size);
    uint64_t result = 0;

    if (op == HB_IR_SHL) {
        result = (ta << shift) & mask;
        ctx->flags.cf = (shift <= msb + 1) ? (((ta >> (msb + 1 - shift)) & 1ULL) != 0) : false;
        ctx->flags.of = (shift == 1) ? ((((result >> msb) & 1ULL) != 0) != ctx->flags.cf) : false;
    } else if (op == HB_IR_SHR) {
        result = ta >> shift;
        ctx->flags.cf = ((ta >> (shift - 1)) & 1ULL) != 0;
        ctx->flags.of = false;
    } else {
        int64_t sa = (int64_t)(ta << (63 - msb)) >> (63 - msb);
        result = (uint64_t)(sa >> shift) & mask;
        ctx->flags.cf = ((ta >> (shift - 1)) & 1ULL) != 0;
        ctx->flags.of = false;
    }

    jit_write_reg(ctx, dst_reg, result);
    ctx->flags.zf = (result == 0);
    ctx->flags.sf = ((result >> msb) & 1ULL) != 0;
    return result;
}

/* --- Public API --- */
struct hb_arm64_codegen {
    hb_context_t* ctx;
};

hb_arm64_codegen_t* hb_arm64_codegen_create(hb_context_t* ctx) {
    hb_arm64_codegen_t* cg = calloc(1, sizeof(hb_arm64_codegen_t));
    if (!cg) return NULL;
    cg->ctx = ctx;
    return cg;
}

void hb_arm64_codegen_destroy(hb_arm64_codegen_t* cg) {
    free(cg);
}

/* MacRunner 2026-06-22: verify-first fix-test for the under-incremented-counter cascade root.
 * Only CMPXCHG/CMPXCHG8B/XCHG/XADD route through the true-atomic helper; the other LOCK-prefixed
 * RMWs (lock inc/dec/add/sub/and/or/xor/bts/btr/btc) currently execute as a DMB-wrapped NON-ATOMIC
 * load-modify-store, so concurrent increments LOSE updates (ABZU's under-incremented CS counter).
 * Env MACRUNNER_HB_LOCK_RMW_ATOMIC serializes those RMWs via the global split-lock — preserving the
 * lazy-flags-correct codegen_instr lowering, just made mutually exclusive — to test whether the
 * c000007b producer-death-deadlock vanishes. Env-gated/default-off (reversible). */
static int hb_lock_rmw_serialize_enabled(void) {
    static int en = -1;
    if (en < 0) en = getenv("MACRUNNER_HB_LOCK_RMW_ATOMIC") ? 1 : 0;
    return en;
}
static bool hb_lock_rmw_uses_atomic_helper(const hb_ir_instr_t* instr) {
    switch (instr->op) {
        case HB_IR_CMPXCHG:
        case HB_IR_CMPXCHG8B:
        case HB_IR_XCHG:
        case HB_IR_XADD:
            return true;
        default:
            return false;
    }
}

hb_result_t hb_arm64_codegen_instr(hb_arm64_codegen_t* cg, const hb_ir_instr_t* instr, hb_codegen_buffer_t* out) {
    bool lk_serialize;
    hb_result_t r;
    if (!instr || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    lk_serialize = instr->is_locked && hb_lock_rmw_serialize_enabled() &&
                   !hb_lock_rmw_uses_atomic_helper(instr);
    if (instr->is_locked) {
        if (lk_serialize) emit_call_helper(out, (void*)hb_jit_split_lock_acquire);
        emit_dmb_ish(out);   /* LOCK-prefixed RMW = full barrier (see block loop) */
    }
    r = codegen_instr(out, instr);
    if (instr->is_locked) {
        if (r == HB_OK) emit_dmb_ish(out);
        if (lk_serialize) emit_call_helper(out, (void*)hb_jit_split_lock_release);
    }
    return r;
}

hb_result_t hb_arm64_codegen_copy_scan_counted_loop(hb_arm64_codegen_t* cg,
                                                    const hb_ir_block_t* body,
                                                    const hb_ir_block_t* guard,
                                                    hb_codegen_buffer_t* out) {
    if (!body || !guard || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_copy_scan_counted_loop_block(out, body, guard) &&
        !emit_two_block_loop_helper(out, body, guard)) {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_bounded_scan_loop(hb_arm64_codegen_t* cg,
                                               const hb_ir_block_t* guard,
                                               const hb_ir_block_t* body,
                                               hb_codegen_buffer_t* out) {
    if (!guard || !body || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_bounded_scan_loop_block(out, guard, body) &&
        !emit_two_block_loop_helper(out, guard, body)) {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_two_block_loop_helper(hb_arm64_codegen_t* cg,
                                                   const hb_ir_block_t* first,
                                                   const hb_ir_block_t* second,
                                                   hb_codegen_buffer_t* out) {
    if (!first || !second || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_two_block_loop_helper(out, first, second))
        return HB_ERR_UNSUPPORTED_FEATURE;
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_four_block_loop_helper(hb_arm64_codegen_t* cg,
                                                    const hb_ir_block_t* first,
                                                    const hb_ir_block_t* second,
                                                    const hb_ir_block_t* third,
                                                    const hb_ir_block_t* fourth,
                                                    hb_codegen_buffer_t* out) {
    if (!first || !second || !third || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_four_block_loop_helper(out, first, second, third, fourth))
        return HB_ERR_UNSUPPORTED_FEATURE;
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_i32_less_tiebreaker_helper(hb_arm64_codegen_t* cg,
                                                        const hb_ir_block_t* entry,
                                                        const hb_ir_block_t* equal,
                                                        const hb_ir_block_t* less,
                                                        hb_codegen_buffer_t* out) {
    if (!entry || !equal || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_i32_less_tiebreaker_helper(out, entry, equal, less))
        return HB_ERR_UNSUPPORTED_FEATURE;
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_unity_sort_inner_loop_helper(hb_arm64_codegen_t* cg,
                                                          const hb_ir_block_t* sort,
                                                          hb_codegen_buffer_t* out) {
    if (!sort || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    emit_prologue(out);
    if (!emit_unity_sort_inner_loop_helper(out, sort))
        return HB_ERR_UNSUPPORTED_FEATURE;
    emit_epilogue(out);
    return HB_OK;
}

static bool codegen_is_control_transfer_op(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

static size_t codegen_instr_limit_before_fallthrough(const hb_ir_block_t* block) {
    if (!block) return 0;
    for (size_t i = 0; i < block->instr_count; i++) {
        if (codegen_is_control_transfer_op(block->instrs[i].op)) return i + 1;
    }
    return block->instr_count;
}

hb_result_t hb_arm64_codegen_block_with_cfg(hb_arm64_codegen_t* cg, const hb_ir_block_t* block,
                                            const hb_ir_cfg_t* cfg, hb_codegen_buffer_t* out) {
    size_t instr_limit;
    bool mid_block_transfer;

    if (!block || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    instr_limit = codegen_instr_limit_before_fallthrough(block);
    mid_block_transfer = instr_limit < block->instr_count;
    emit_prologue(out);
    {
        const char* trace_env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
        const char* watch_env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_ADDR");
        uint64_t watch = watch_env && *watch_env ? strtoull(watch_env, NULL, 0) : 0;
        if (trace_env && *trace_env && *trace_env != '0' && watch && block->guest_addr == watch) {
            fprintf(stderr,
                    "macrunner-hb-codegen-watch: guest=%p instrs=%zu instr_limit=%zu mid=%u unity_sort_candidate=%u\n",
                    (void*)(uintptr_t)block->guest_addr, block->instr_count, instr_limit,
                    mid_block_transfer ? 1u : 0u,
                    unity_sort_inner_loop_candidate(block) ? 1u : 0u);
            fflush(stderr);
        }
    }
    if (!mid_block_transfer && block->instr_count == 7) {
        if (emit_unity_sort_inner_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
    }
    if (!mid_block_transfer && cfg && block->instr_count == 5) {
        const hb_ir_instr_t* jcc = &block->instrs[4];
        const hb_ir_block_t* guard = (jcc->op == HB_IR_Jcc)
            ? find_cfg_block_for_codegen(cfg, jcc->guest_addr + jcc->guest_len)
            : NULL;
        if (emit_copy_scan_counted_loop_block(out, block, guard)) {
            emit_epilogue(out);
            return HB_OK;
        }
    }
    if (!mid_block_transfer) {
        if (emit_unity_freelist_fill_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_unity_u32_ptr_compare(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_string_hash(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_string_equal(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_coded_index_search(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_rowptr_entry(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_decode_row_entry(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_decode_col(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_decode_row_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_mono_metadata_bsearch_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_unity_string_bsearch_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (!jit_direct_mem_codegen_enabled(out) && emit_hot_helper_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_store_count_loop_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_prologue_local_init_test_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_epilogue_restore_ret_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_zero_store_update_backedge_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_copy_scan_body_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_hot_scalar_scan_loop(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
        if (emit_zero_test_jcc_block(out, block)) {
            emit_epilogue(out);
            return HB_OK;
        }
    }
    for (size_t i = 0; i < instr_limit; i++) {
        if (i + 2 < instr_limit &&
            emit_arith_rmw_dead_flags_test_jcc(out, &block->instrs[i], &block->instrs[i + 1],
                                               &block->instrs[i + 2])) {
            i += 2;
            continue;
        }
        if (i + 3 < instr_limit &&
            emit_stack_spill_push_sub_prologue(out, &block->instrs[i], &block->instrs[i + 1],
                                               &block->instrs[i + 2], &block->instrs[i + 3])) {
            i += 3;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_xfg_dispatch_call_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 2 < instr_limit &&
            emit_store_imm_mov_lea_same_base(out, &block->instrs[i], &block->instrs[i + 1],
                                             &block->instrs[i + 2])) {
            i += 2;
            continue;
        }
        if (i + 2 < instr_limit &&
            emit_scalar_flags_setcc_sequence(out, &block->instrs[i], &block->instrs[i + 1],
                                             &block->instrs[i + 2])) {
            i += 2;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_scalar_flags_setcc_sequence(out, &block->instrs[i], NULL, &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_mov_lea_same_base_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_xmm_load_store_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_scalar_load_store_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_adjacent_mem64_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_mem_reg_flags_jcc_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_mem_imm_flags_jcc_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_test_same_reg_jcc_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        if (i + 1 < instr_limit &&
            emit_scalar_flags_jcc_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        /* MacRunner Lane A (2026-06-17): x86 LOCK-prefixed RMW (lock or/and/add/...) acts as a
         * full memory barrier.  block_has_atomic_ir only routes CMPXCHG/XCHG/XADD through the
         * DMB-bracketed atomic helper, so bracket every other lock-flagged op with DMB ISH here
         * — Mono hazard-pointer loops (`lock or [rsp],r`) livelock on ARM64 without it. */
        const hb_ir_instr_t* lk_instr = &block->instrs[i];
        if (lk_instr->is_locked) emit_dmb_ish(out);
        hb_result_t r = codegen_instr(out, lk_instr);
        if (r != HB_OK) return r;
        if (lk_instr->is_locked) emit_dmb_ish(out);
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_block(hb_arm64_codegen_t* cg, const hb_ir_block_t* block, hb_codegen_buffer_t* out) {
    return hb_arm64_codegen_block_with_cfg(cg, block, NULL, out);
}

hb_result_t hb_arm64_codegen_func(hb_arm64_codegen_t* cg, const hb_ir_func_t* func, hb_codegen_buffer_t* out) {
    if (!func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    if (cg && cg->ctx) out->arch = cg->ctx->arch;
    for (size_t i = 0; i < func->cfg->block_count; i++) {
        hb_result_t r = hb_arm64_codegen_block(cg, func->cfg->blocks[i], out);
        if (r != HB_OK) return r;
    }
    return HB_OK;
}
