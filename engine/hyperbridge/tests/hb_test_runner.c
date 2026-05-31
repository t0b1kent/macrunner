/* HyperBridge C test runner */
#include "hb_context.h"
#include "hb_decoder.h"
#include "hb_lifter.h"
#include "hb_ir.h"
#include "hb_runtime.h"
#include "hb_codegen.h"
#include "hb_memory.h"
#include "hb_x87.h"
#include "hb_wow64cpu.h"
#include "hb_flags.h"
#include "hb_result.h"
#include "hb_cache.h"
#include "hb_abi.h"
#include "hb_marker.h"
#include "hb_iat.h"
#include "hb_fault.h"
#include "hb_thunk.h"
#if defined(__has_include)
#  if __has_include("hb_pe.h")
#    include "hb_pe.h"
#  else
#    include <stdint.h>

typedef struct hb_pe_image_t {
    uint32_t number_of_sections;
    uint8_t* mapped_image;
    size_t mapped_size;
    size_t size_of_image;
} hb_pe_image_t;

hb_pe_image_t* hb_pe_load(uint8_t* data, size_t size);
hb_result_t hb_pe_map_image(hb_pe_image_t* pe, int flags);
void hb_pe_unload(hb_pe_image_t* pe);
#  endif
#else
#  include "hb_pe.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

static int tests_passed = 0;
static int tests_failed = 0;

static uint64_t hb_test_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); tests_failed++; return; } } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: expected %llu, got %llu\n", __FILE__, __LINE__, (unsigned long long)(b), (unsigned long long)(a)); tests_failed++; return; } } while(0)

static char* save_env_var(const char* name) {
    const char* value = getenv(name);
    return value ? strdup(value) : NULL;
}

static void restore_env_var(const char* name, char* saved) {
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

/* Test memory note: hb_memory_map() over an arbitrary guest address has no
 * host backing in this harness. For string-op tests that read/write bytes,
 * map real local buffers or add an explicit backed-memory helper first. */

struct special_write_fixture {
    hb_gva_t addr;
    uint8_t* bytes;
    size_t size;
};

static hb_result_t special_write_fixture_cb(void* user, hb_gva_t addr, const void* in, size_t size) {
    struct special_write_fixture* fix = (struct special_write_fixture*)user;
    if (!fix || !in) return HB_ERR_MEMORY_FAULT;
    if (addr < fix->addr || addr + size > fix->addr + fix->size) return HB_ERR_MEMORY_FAULT;
    memcpy(fix->bytes + (addr - fix->addr), in, size);
    return HB_OK;
}

static void materialize_shift_flags(hb_context_t* ctx, uint64_t raw_count) {
    uint64_t count = raw_count & 0x3FULL;
    uint32_t mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF | HB_FLAG_BIT_PF;
    if (count == 1) mask |= HB_FLAG_BIT_OF;
    hb_lazy_flags_materialize(ctx, mask);
}

static uint64_t test_double_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t test_float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int decode_riprel_target(const uint8_t* code, size_t len, uint64_t base,
                                int slot, uint64_t* rip_base, uint64_t* target) {
    hb_decoded_t d;
    if (hb_decode_x64(code, len, base, &d) != HB_OK) return 0;
    if (slot == 1) {
        if (!d.op1.is_mem || !d.op1.mem.rip_relative) return 0;
        *rip_base = d.op1.mem.rip_target;
        *target = (uint64_t)((int64_t)d.op1.mem.rip_target + d.op1.mem.disp);
    } else if (slot == 2) {
        if (!d.op2.is_mem || !d.op2.mem.rip_relative) return 0;
        *rip_base = d.op2.mem.rip_target;
        *target = (uint64_t)((int64_t)d.op2.mem.rip_target + d.op2.mem.disp);
    } else {
        if (!d.op3.is_mem || !d.op3.mem.rip_relative) return 0;
        *rip_base = d.op3.mem.rip_target;
        *target = (uint64_t)((int64_t)d.op3.mem.rip_target + d.op3.mem.disp);
    }
    return 1;
}

/* --- Decode tests --- */
TEST(decode_mov_reg_reg) {
    uint8_t code[] = {0x48, 0x89, 0xc8}; /* mov rax, rcx */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.reg == 0); /* rax */
    ASSERT(d.op2.reg == 1); /* rcx */
    tests_passed++;
}

TEST(decode_mov_r8_mem16_operand_override) {
    uint8_t code[] = {0x66, 0x41, 0x89, 0x00}; /* mov word ptr [r8], ax */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x14040493f, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op1.mem.base == HB_REG_R8);
    ASSERT(d.op2.is_reg);
    ASSERT(d.op2.reg == HB_REG_RAX);
    ASSERT(d.op2.size == 2);
    tests_passed++;
}

TEST(decode_x64_mov_moffs_family) {
    uint8_t load[] = {0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    uint8_t store32[] = {0x67, 0xA2, 0x44, 0x33, 0x22, 0x11};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(load, sizeof(load), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == sizeof(load));
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1 && d.op2.mem.index == -1);
    ASSERT(d.op2.mem.disp == (int64_t)0x1122334455667788LL);
    ASSERT(d.op2.size == 8);

    ASSERT(hb_decode_x64(store32, sizeof(store32), 0x2000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == sizeof(store32));
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.index == -1);
    ASSERT(d.op1.mem.addr32);
    ASSERT(d.op1.mem.disp == 0x11223344);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 1);
    tests_passed++;
}

TEST(decode_x64_xchg_accumulator_opcode_family) {
    uint8_t nop[] = {0x90};
    uint8_t xchg_rcx[] = {0x48, 0x91};
    uint8_t xchg_r8[] = {0x49, 0x90};
    uint8_t xchg_ax[] = {0x66, 0x92};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(nop, sizeof(nop), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);

    ASSERT(hb_decode_x64(xchg_rcx, sizeof(xchg_rcx), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG);
    ASSERT(d.len == sizeof(xchg_rcx));
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.size == 8);

    ASSERT(hb_decode_x64(xchg_r8, sizeof(xchg_r8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_R8 && d.op2.size == 8);

    ASSERT(hb_decode_x64(xchg_ax, sizeof(xchg_ax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 2);
    tests_passed++;
}

TEST(decode_x64_segment_mov_leave_family) {
    uint8_t mov_rax_es[] = {0x48, 0x8C, 0xC0}; /* mov rax, es */
    uint8_t mov_gs_rax[] = {0x48, 0x8E, 0xE8}; /* mov gs, rax */
    uint8_t leave64[] = {0xC9};
    uint8_t leave16[] = {0x66, 0xC9};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(mov_rax_es, sizeof(mov_rax_es), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG);
    ASSERT(d.len == sizeof(mov_rax_es));
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.is_imm && d.op2.imm == 0 && d.op2.size == 2);

    ASSERT(hb_decode_x64(mov_gs_rax, sizeof(mov_gs_rax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG);
    ASSERT(d.len == sizeof(mov_gs_rax));
    ASSERT(d.op1.is_imm && d.op1.imm == 5 && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 8);

    ASSERT(hb_decode_x64(leave64, sizeof(leave64), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LEAVE);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBP && d.op1.size == 8);
    ASSERT(d.stack_delta == 8);

    ASSERT(hb_decode_x64(leave16, sizeof(leave16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LEAVE);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBP && d.op1.size == 2);
    ASSERT(d.stack_delta == 2);
    tests_passed++;
}

TEST(decode_x64_port_interrupt_flag_family) {
    uint8_t insd[] = {0x6D};
    uint8_t out_dx_eax[] = {0xEF};
    uint8_t in_al_imm[] = {0xE4, 0x64};
    uint8_t retf_imm[] = {0xCA, 0x10, 0x00};
    uint8_t enter[] = {0xC8, 0x20, 0x00, 0x02};
    uint8_t int3[] = {0xCC};
    uint8_t int_imm[] = {0xCD, 0x2E};
    uint8_t clc[] = {0xF8};
    uint8_t std[] = {0xFD};
    uint8_t xlat[] = {0xD7};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(insd, sizeof(insd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INS && d.op1.reg == HB_REG_RDI && d.op1.size == 4);
    ASSERT(d.op2.reg == HB_REG_RDX && d.op2.size == 2);

    ASSERT(hb_decode_x64(out_dx_eax, sizeof(out_dx_eax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_OUT && d.op1.reg == HB_REG_RDX && d.op2.reg == HB_REG_RAX);
    ASSERT(d.op2.size == 4);

    ASSERT(hb_decode_x64(in_al_imm, sizeof(in_al_imm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IN && d.op1.reg == HB_REG_RAX && d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x64);

    ASSERT(hb_decode_x64(retf_imm, sizeof(retf_imm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RETF && d.is_ret && d.ret_imm == 0x10);

    ASSERT(hb_decode_x64(enter, sizeof(enter), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ENTER && d.op1.imm == 0x20 && d.op2.imm == 2);

    ASSERT(hb_decode_x64(int3, sizeof(int3), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INT3 && d.is_branch);

    ASSERT(hb_decode_x64(int_imm, sizeof(int_imm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INT && d.op1.imm == 0x2E && d.is_branch);

    ASSERT(hb_decode_x64(clc, sizeof(clc), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CLC && d.writes_flags);

    ASSERT(hb_decode_x64(std, sizeof(std), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_STD && d.writes_flags);

    ASSERT(hb_decode_x64(xlat, sizeof(xlat), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XLAT && d.op1.reg == HB_REG_RAX && d.op1.size == 1);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RBX && d.op2.mem.index == HB_REG_RAX);
    tests_passed++;
}

TEST(decode_x64_0f_system_segment_family) {
    uint8_t mov_cr0_rax[] = {0x0F, 0x22, 0xC0};
    uint8_t mov_rax_dr0[] = {0x0F, 0x21, 0xC0};
    uint8_t push_fs[] = {0x0F, 0xA0};
    uint8_t pop_gs[] = {0x0F, 0xA9};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(mov_cr0_rax, sizeof(mov_cr0_rax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_CR);
    ASSERT(d.op1.is_imm && d.op1.imm == 0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX);

    ASSERT(hb_decode_x64(mov_rax_dr0, sizeof(mov_rax_dr0), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_DR);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op2.is_imm && d.op2.imm == 0);

    ASSERT(hb_decode_x64(push_fs, sizeof(push_fs), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSH_SEG && d.op1.imm == 4);

    ASSERT(hb_decode_x64(pop_gs, sizeof(pop_gs), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP_SEG && d.op1.imm == 5);
    tests_passed++;
}

TEST(decode_x64_x87_pushf_rotate_family) {
    uint8_t fadd_st0_st1[] = {0xD8, 0xC1};
    uint8_t fdivp_st1_st0[] = {0xDE, 0xF9};
    uint8_t fcmovb_st0_st1[] = {0xDA, 0xC1};
    uint8_t fstp_st1[] = {0xDD, 0xD9};
    uint8_t pushfq[] = {0x9C};
    uint8_t popfw[] = {0x66, 0x9D};
    uint8_t rcl_rax_1[] = {0x48, 0xD1, 0xD0};
    uint8_t rcr_al_3[] = {0xC0, 0xD8, 0x03};
    uint8_t cli[] = {0xFA};
    uint8_t sti[] = {0xFB};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(fadd_st0_st1, sizeof(fadd_st0_st1), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FADD && d.op1.is_imm && d.op1.imm == 1);

    ASSERT(hb_decode_x64(fdivp_st1_st0, sizeof(fdivp_st1_st0), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FDIVP && d.op1.is_imm && d.op1.imm == 1);

    ASSERT(hb_decode_x64(fcmovb_st0_st1, sizeof(fcmovb_st0_st1), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FCMOV && d.op1.is_imm && d.op1.imm == 1);

    ASSERT(hb_decode_x64(fstp_st1, sizeof(fstp_st1), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FSTP && d.op1.is_imm && d.op1.imm == 1);

    ASSERT(hb_decode_x64(pushfq, sizeof(pushfq), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSHF && d.stack_delta == -8 && d.op1.imm == 8);

    ASSERT(hb_decode_x64(popfw, sizeof(popfw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POPF && d.stack_delta == 8 && d.op1.imm == 2);

    ASSERT(hb_decode_x64(rcl_rax_1, sizeof(rcl_rax_1), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RCL && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.imm == 1);

    ASSERT(hb_decode_x64(rcr_al_3, sizeof(rcr_al_3), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RCR && d.op1.reg == HB_REG_RAX && d.op1.size == 1);
    ASSERT(d.op2.imm == 3);

    ASSERT(hb_decode_x64(cli, sizeof(cli), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CLI && d.writes_flags);

    ASSERT(hb_decode_x64(sti, sizeof(sti), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_STI && d.writes_flags);
    tests_passed++;
}

TEST(decode_x64_prefix_pop_hlt_family) {
    uint8_t mixed_prefix_sar[] = {0x48, 0x26, 0xC0, 0x7F, 0x34, 0x12};
    uint8_t pop_rax[] = {0x8F, 0xC0};
    uint8_t pop_ax[] = {0x66, 0x8F, 0xC0};
    uint8_t hlt[] = {0xF4};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(mixed_prefix_sar, sizeof(mixed_prefix_sar), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SAR);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x12);

    ASSERT(hb_decode_x64(pop_rax, sizeof(pop_rax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP && d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);

    ASSERT(hb_decode_x64(pop_ax, sizeof(pop_ax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP && d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);

    ASSERT(hb_decode_x64(hlt, sizeof(hlt), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_HLT);
    tests_passed++;
}

TEST(decode_x64_0f_unsupported_family_lengths) {
    uint8_t syscall[] = {0x0F, 0x05};
    uint8_t ud2[] = {0x0F, 0x0B};
    uint8_t sldt[] = {0x0F, 0x00, 0xC0};
    uint8_t femms[] = {0x0F, 0x0E};
    uint8_t montmul[] = {0x0F, 0xA6, 0xC0};
    uint8_t ud1[] = {0x0F, 0xB9, 0xC0};
    uint8_t mmx_movq[] = {0x0F, 0x6F, 0xC0};
    uint8_t mmx_paddd[] = {0x0F, 0xFE, 0xC0};
    uint8_t mmx_pshufw[] = {0x0F, 0x70, 0xC0, 0x7F};
    uint8_t vmread[] = {0x0F, 0x78, 0xC0};
    uint8_t haddpd[] = {0x66, 0x0F, 0x7C, 0xC0};
    uint8_t insertq[] = {0xF2, 0x0F, 0x78, 0xC0, 0x7F, 0x34};
    uint8_t rdfsbase[] = {0xF3, 0x0F, 0xAE, 0xC0};
    uint8_t popcnt[] = {0xF3, 0x0F, 0xB8, 0xC0};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(syscall, sizeof(syscall), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 2);

    ASSERT(hb_decode_x64(ud2, sizeof(ud2), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UD && d.len == 2);

    ASSERT(hb_decode_x64(sldt, sizeof(sldt), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 3);

    ASSERT(hb_decode_x64(femms, sizeof(femms), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MMX && d.len == 2);

    ASSERT(hb_decode_x64(montmul, sizeof(montmul), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 3);

    ASSERT(hb_decode_x64(ud1, sizeof(ud1), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UD && d.len == 2);

    ASSERT(hb_decode_x64(mmx_movq, sizeof(mmx_movq), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MMX && d.len == 3);

    ASSERT(hb_decode_x64(mmx_paddd, sizeof(mmx_paddd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MMX && d.len == 3);

    ASSERT(hb_decode_x64(mmx_pshufw, sizeof(mmx_pshufw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MMX && d.len == 4 && d.op3.imm == 0x7F);

    ASSERT(hb_decode_x64(vmread, sizeof(vmread), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 3);

    ASSERT(hb_decode_x64(haddpd, sizeof(haddpd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 4);

    ASSERT(hb_decode_x64(insertq, sizeof(insertq), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 6 && d.op3.imm == 0x7F);

    ASSERT(hb_decode_x64(rdfsbase, sizeof(rdfsbase), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 4);

    ASSERT(hb_decode_x64(popcnt, sizeof(popcnt), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SYS && d.len == 4);
    tests_passed++;
}

TEST(decode_x64_operand16_immediate_lengths) {
    uint8_t pushw[] = {0x66, 0x68, 0xC0, 0x7F, 0x34, 0x12};
    uint8_t imulw[] = {0x66, 0x69, 0xC0, 0x7F, 0x34, 0x12};
    uint8_t testw[] = {0x66, 0xA9, 0xC0, 0x7F, 0x34, 0x12};
    uint8_t jow[] = {0x66, 0x0F, 0x80, 0xC0, 0x7F, 0x34, 0x12};
    uint8_t jbw[] = {0x66, 0x0F, 0x82, 0xC0, 0x7F, 0x34, 0x12};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(pushw, sizeof(pushw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSH && d.len == 4 && d.op1.size == 2);

    ASSERT(hb_decode_x64(imulw, sizeof(imulw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IMUL && d.len == 5 && d.op1.size == 2 && d.op3.size == 2);

    ASSERT(hb_decode_x64(testw, sizeof(testw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST && d.len == 4 && d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x64(jow, sizeof(jow), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_Jcc && d.len == 5);

    ASSERT(hb_decode_x64(jbw, sizeof(jbw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_Jcc && d.len == 7);
    tests_passed++;
}

TEST(decode_x64_0f38_0f3a_vector_family_lengths) {
    uint8_t phaddw[] = {0x0F, 0x38, 0x01, 0xC0};
    uint8_t palignr[] = {0x0F, 0x3A, 0x0F, 0xC0, 0x7F};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(phaddw, sizeof(phaddw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 4);

    ASSERT(hb_decode_x64(palignr, sizeof(palignr), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 5 && d.op3.imm == 0x7F);
    tests_passed++;
}

TEST(decode_x64_vex_evex_vector_family_lengths) {
    uint8_t vzeroupper[] = {0xC5, 0xF8, 0x77};
    uint8_t vaddps[] = {0xC5, 0xFC, 0x58, 0xC0};
    uint8_t evex_vaddps[] = {0x62, 0xF1, 0x7C, 0x48, 0x58, 0xC0};
    hb_decoded_t d;

    ASSERT(hb_decode_x64(vzeroupper, sizeof(vzeroupper), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 3);

    ASSERT(hb_decode_x64(vaddps, sizeof(vaddps), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 4);

    ASSERT(hb_decode_x64(evex_vaddps, sizeof(evex_vaddps), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_VEC && d.len == 6);
    tests_passed++;
}

TEST(decode_add_imm) {
    uint8_t code[] = {0x48, 0x83, 0xc0, 0x01}; /* add rax, 1 */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_ADD);
    ASSERT(d.op1.reg == 0); /* rax */
    ASSERT(d.op2.imm == 1);
    tests_passed++;
}

TEST(decode_x64_accumulator_imm_logic_family) {
    struct {
        uint8_t opcode;
        int insn;
    } cases[] = {
        {0x0c, HB_INS_OR},  {0x0d, HB_INS_OR},
        {0x24, HB_INS_AND}, {0x25, HB_INS_AND},
        {0x34, HB_INS_XOR}, {0x35, HB_INS_XOR},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code[5] = { cases[i].opcode, 0x78, 0x56, 0x34, 0x12 };
        size_t len = (cases[i].opcode & 1) ? 5 : 2;
        hb_decoded_t d;
        hb_result_t r = hb_decode_x64(code, len, 0x7ffd0232c7bULL, &d);
        ASSERT(r == HB_OK);
        ASSERT((int)d.opcode == cases[i].insn);
        ASSERT(d.len == len);
        ASSERT(d.op1.is_reg);
        ASSERT(d.op1.reg == HB_REG_RAX);
        ASSERT(d.op1.size == ((cases[i].opcode & 1) ? 4 : 1));
        ASSERT(d.op2.is_imm);
    }
    tests_passed++;
}

TEST(interp_x64_and_eax_imm32_calc_callback) {
    uint8_t code[] = {0x25, 0xff, 0x00, 0x00, 0x00}; /* and eax, 0xff */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xffffffffffff1234ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x34);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->flags.sf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_linear_block_advances_pc) {
    uint8_t code[] = {0x48, 0x89, 0xc8}; /* mov rax, rcx */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rip = base;
    ctx->regs.x64.rcx = 0x12345678;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x12345678);
    ASSERT(ctx->pc == base + sizeof(code));
    ASSERT(ctx->regs.x64.rip == base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(memory_special_write_unmapped_live_range) {
    uint8_t backing[16] = {0};
    struct special_write_fixture fix = {0x109840050ULL, backing, sizeof(backing)};
    hb_memory_t* mem = hb_memory_create(0);
    ASSERT(mem != NULL);

    hb_memory_set_special_handlers(mem, NULL, special_write_fixture_cb, &fix);
    ASSERT(hb_memory_write_u64(mem, 0x109840050ULL, 0x1122334455667788ULL) == HB_OK);
    ASSERT(backing[0] == 0x88);
    ASSERT(backing[7] == 0x11);
    ASSERT(hb_memory_write_u64(mem, 0x109840060ULL, 0) == HB_ERR_MEMORY_FAULT);

    hb_memory_destroy(mem);
    tests_passed++;
}

TEST(memory_cross_region_write_read_span) {
    const size_t page = 4096;
    uint8_t* backing = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(backing != MAP_FAILED);

    hb_memory_t* mem = hb_memory_create(0);
    ASSERT(mem != NULL);
    ASSERT(hb_memory_map(mem, (hb_gva_t)(uintptr_t)backing, page,
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(mem, (hb_gva_t)(uintptr_t)(backing + page), page,
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    uint8_t src[16], dst[16] = {0};
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(0xa0 + i);

    hb_gva_t addr = (hb_gva_t)(uintptr_t)(backing + page - 5);
    ASSERT(!hb_memory_can_write(mem, addr, sizeof(src)));
    ASSERT(hb_memory_can_write_span(mem, addr, sizeof(src)));
    ASSERT(hb_memory_write(mem, addr, src, sizeof(src)) == HB_OK);
    ASSERT(memcmp(backing + page - 5, src, sizeof(src)) == 0);
    ASSERT(hb_memory_read(mem, addr, dst, sizeof(dst)) == HB_OK);
    ASSERT(memcmp(dst, src, sizeof(src)) == 0);

    hb_memory_destroy(mem);
    munmap(backing, page * 2);
    tests_passed++;
}

TEST(memory_private_guest_low_va_backing) {
    hb_memory_t* mem = hb_memory_create(0);
    const hb_gva_t guest = 0x400000;
    uint32_t value = 0;

    ASSERT(mem != NULL);
    ASSERT(hb_memory_map_private(mem, guest, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    hb_region_t* r = hb_memory_find_region(mem, guest);
    ASSERT(r != NULL);
    ASSERT(r->base == guest);
    ASSERT(r->allocated);
    ASSERT(r->host_base != NULL);
    ASSERT((hb_gva_t)(uintptr_t)r->host_base != guest);

    ASSERT(hb_memory_write_u32(mem, guest + 0x100, 0xaabbccdd) == HB_OK);
    ASSERT(hb_memory_read_u32(mem, guest + 0x100, &value) == HB_OK);
    ASSERT(value == 0xaabbccdd);
    ASSERT(hb_memory_unmap(mem, guest) == HB_OK);

    hb_memory_destroy(mem);
    tests_passed++;
}

TEST(memory_guest32_window_direct_mapping) {
    hb_memory_t* mem = hb_memory_create(0);
    const uint32_t guest = 0x00400000u;
    uint32_t value = 0;

    ASSERT(mem != NULL);
    ASSERT(hb_memory_guest32_reserve(mem) == HB_OK);
    ASSERT(hb_memory_guest32_base(mem) != NULL);
    ASSERT(((uintptr_t)hb_memory_guest32_base(mem) & 0xffffffffULL) == 0);
    ASSERT(hb_memory_guest32_map(mem, guest, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    void* host = hb_memory_guest32_to_host(mem, guest + 0x80);
    ASSERT(host != NULL);
    ASSERT((uintptr_t)host == (uintptr_t)hb_memory_guest32_base(mem) + guest + 0x80);

    ASSERT(hb_memory_write_u32(mem, guest + 0x80, 0x13572468u) == HB_OK);
    ASSERT(hb_memory_read_u32(mem, guest + 0x80, &value) == HB_OK);
    ASSERT(value == 0x13572468u);

    hb_memory_destroy(mem);
    tests_passed++;
}

TEST(memory_guest32_host_mirror_alias_read_write) {
    hb_memory_t* mem = hb_memory_create(0);
    const uint32_t guest = 0x00500000u;
    uint32_t value = 0x11223344u, out = 0;
    hb_gva_t host_alias;

    ASSERT(mem != NULL);
    ASSERT(hb_memory_guest32_reserve(mem) == HB_OK);
    ASSERT(hb_memory_guest32_base(mem) != NULL);
    ASSERT(hb_memory_guest32_map(mem, guest, 4096, HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);

    host_alias = (hb_gva_t)(uintptr_t)hb_memory_guest32_base(mem) + guest + 0x6f;
    ASSERT(hb_memory_write(mem, host_alias, &value, sizeof(value)) == HB_OK);
    ASSERT(hb_memory_read_u32(mem, guest + 0x6f, &out) == HB_OK);
    ASSERT(out == value);
    ASSERT(hb_memory_write_u32(mem, guest + 0x6f, 0x2468ace0u) == HB_OK);
    ASSERT(hb_memory_read(mem, host_alias, &out, sizeof(out)) == HB_OK);
    ASSERT(out == 0x2468ace0u);

    hb_memory_destroy(mem);
    tests_passed++;
}

TEST(memory_guest32_write_repairs_stale_host_protection) {
#ifdef __APPLE__
    hb_memory_t* mem = hb_memory_create(0);
    const uint32_t guest = 0x00500000u;
    uint8_t value = 0xaa;
    uint8_t out = 0;
    void* host;
    long page_size;
    uintptr_t page;

    ASSERT(mem != NULL);
    ASSERT(hb_memory_guest32_map(mem, guest, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    host = hb_memory_guest32_to_host(mem, guest + 0x6f);
    ASSERT(host != NULL);

    page_size = sysconf(_SC_PAGESIZE);
    ASSERT(page_size > 0);
    page = (uintptr_t)host & ~(uintptr_t)(page_size - 1);
    ASSERT(mprotect((void*)page, (size_t)page_size, PROT_READ | PROT_EXEC) == 0);
    ASSERT(hb_memory_write(mem, guest + 0x6f, &value, sizeof(value)) == HB_OK);
    ASSERT(hb_memory_read(mem, guest + 0x6f, &out, sizeof(out)) == HB_OK);
    ASSERT(out == value);

    ASSERT(hb_memory_guest32_protect(mem, guest, 4096, HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(mem, guest + 0x6f, &value, sizeof(value)) == HB_ERR_MEMORY_FAULT);

    hb_memory_destroy(mem);
#endif
    tests_passed++;
}

TEST(memory_guest32_exec_generation_on_write_protect_unmap) {
    hb_memory_t* mem = hb_memory_create(0);
    const uint32_t guest = 0x00800000u;

    ASSERT(mem != NULL);
    ASSERT(hb_memory_guest32_map(mem, guest, 4096 * 3,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    uint64_t gen_after_map = hb_memory_generation(mem);
    ASSERT(gen_after_map != 0);

    ASSERT(hb_memory_write_u32(mem, guest + 0x100, 0xaabbccddu) == HB_OK);
    uint64_t gen_after_write = hb_memory_generation(mem);
    ASSERT(gen_after_write > gen_after_map);

    ASSERT(hb_memory_guest32_protect(mem, guest + 4096, 4096, HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_generation(mem) > gen_after_write);
    ASSERT(hb_memory_write_u32(mem, guest + 4096 + 0x10, 0x11223344u) == HB_ERR_MEMORY_FAULT);
    ASSERT(hb_memory_write_u32(mem, guest + 0x10, 0x55667788u) == HB_OK);

    ASSERT(hb_memory_guest32_unmap(mem, guest + 4096, 4096) == HB_OK);
    ASSERT(hb_memory_guest32_to_host(mem, guest + 4096) == NULL);
    ASSERT(hb_memory_guest32_to_host(mem, guest) != NULL);
    ASSERT(hb_memory_guest32_to_host(mem, guest + 8192) != NULL);
    ASSERT(hb_memory_guest32_protect(mem, guest + 4096, 4096, HB_PERM_READ) == HB_ERR_NOT_FOUND);

    hb_memory_destroy(mem);
    tests_passed++;
}

TEST(interp_x86_guest32_memory_operands_wrap_to_direct_window) {
    const uint32_t code_base = 0x00401000u;
    const uint32_t data_base = 0x00000000u;
    uint8_t code[] = {
        0x8b, 0x83, 0x24, 0x00, 0x00, 0x00, /* mov eax, [ebx+0x24] */
        0x89, 0x83, 0x28, 0x00, 0x00, 0x00  /* mov [ebx+0x28], eax */
    };
    uint32_t value = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, 0x00000004u, 0xdecafbadu) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = 0xffffffe0u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0xdecafbadu);
    ASSERT(hb_memory_read_u32(ctx->memory, 0x00000008u, &value) == HB_OK);
    ASSERT(value == 0xdecafbadu);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_interp_x86_fs_absolute_store_advances_past_disp32) {
    const uint32_t code_base = 0x00402000u;
    const uint32_t fs_base = 0x7f000000u;
    uint8_t code[] = {
        0x64, 0x89, 0x0d, 0x00, 0x00, 0x00, 0x00, /* mov fs:[0], ecx */
    };
    hb_decoded_t d;
    uint32_t value = 0;

    ASSERT(hb_decode_x86(code, sizeof(code), code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == sizeof(code));
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.mem.base == -1);
    ASSERT(d.op1.mem.disp == 0);
    ASSERT(d.op1.mem.segment == 0x64);
    ASSERT(d.op2.is_reg);
    ASSERT(d.op2.reg == HB_REG_RCX);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, fs_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ecx = 0x1234abcdu;
    ctx->fs_base = fs_base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(!out.faulted);
    ASSERT(ctx->pc == code_base + sizeof(code));
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));
    ASSERT(hb_memory_read_u32(ctx->memory, fs_base, &value) == HB_OK);
    ASSERT(value == 0x1234abcdu);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_call_boundary_exports_eip) {
    const uint32_t code_base = 0x00401000u;
    const uint32_t target = 0x00402000u;
    const uint32_t stack_top = 0x00620000u;
    uint8_t code[] = {0xe8, 0xfb, 0x0f, 0x00, 0x00}; /* call 0x00402000 */
    uint32_t ret = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_top - 4096, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = stack_top;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == target);
    ASSERT(ctx->regs.x86.eip == target);
    ASSERT(ctx->regs.x86.esp == stack_top - 4);
    ASSERT(hb_memory_read_u32(ctx->memory, stack_top - 4, &ret) == HB_OK);
    ASSERT(ret == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_ret_imm16_family) {
    hb_decoded_t d;
    uint8_t ret_near[] = {0xc3};
    uint8_t ret_imm[] = {0xc2, 0x34, 0x12};

    ASSERT(hb_decode_x86(ret_near, sizeof(ret_near), 0x00401000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RET);
    ASSERT(d.is_ret);
    ASSERT(d.ret_imm == 0);

    ASSERT(hb_decode_x86(ret_imm, sizeof(ret_imm), 0x00402000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RET);
    ASSERT(d.is_ret);
    ASSERT(d.ret_imm == 0x1234u);

    ASSERT(hb_decode_x64(ret_imm, sizeof(ret_imm), 0x140002000ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_RET);
    ASSERT(d.is_ret);
    ASSERT(d.ret_imm == 0x1234u);

    tests_passed++;
}

TEST(interp_x86_ret_imm16_cleans_stdcall_stack) {
    const uint32_t code_base = 0x00403000u;
    const uint32_t ret_target = 0x00404000u;
    const uint32_t stack_top = 0x00630000u;
    uint8_t code[] = {0xc2, 0x08, 0x00}; /* ret 8 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_top - 4096, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, stack_top - 12, ret_target) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = stack_top - 12;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == ret_target);
    ASSERT(ctx->regs.x86.eip == ret_target);
    ASSERT(ctx->regs.x86.esp == stack_top);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_ret_imm16_uses_guest_return_slot) {
    const uint32_t code_base = 0x7a93255cu;
    const uint32_t ret_target = 0x7b145d74u;
    const uint32_t stack_page = 0x01b5e000u;
    const uint32_t ret_esp = 0x01b5edb8u;
    uint8_t code[] = {0xc2, 0x10, 0x00}; /* ret 0x10 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base & ~0xfffu, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_page, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ret_esp, ret_target) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ret_esp + 4, 0x0001004eu) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ret_esp + 8, 0x01b5efd4u) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ret_esp + 12, 0x01b5efdcu) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ret_esp + 16, 0x01b5f020u) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = ret_esp;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == ret_target);
    ASSERT(ctx->regs.x86.eip == ret_target);
    ASSERT(ctx->regs.x86.esp == ret_esp + 4 + 0x10);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_ret_imm16_cleans_stack) {
    struct {
        uint8_t code_page[4096];
        uint8_t stack_page[4096];
    } backing;
    const uint64_t code_base = (uint64_t)(uintptr_t)backing.code_page;
    const uint64_t ret_target = 0x140004000ULL;
    const uint64_t stack_top = (uint64_t)(uintptr_t)(backing.stack_page + sizeof(backing.stack_page));
    uint8_t code[] = {0xc2, 0x20, 0x00}; /* ret 0x20 */
    memset(&backing, 0, sizeof(backing));

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, code_base, sizeof(backing.code_page),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)backing.stack_page, sizeof(backing.stack_page),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u64(ctx->memory, stack_top - 0x28, ret_target) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x64.rip = code_base;
    ctx->regs.x64.rsp = stack_top - 0x28;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == ret_target);
    ASSERT(ctx->regs.x64.rip == ret_target);
    ASSERT(ctx->regs.x64.rsp == stack_top);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_external_jcc_boundary_exports_eip) {
    const uint32_t code_base = 0x00401800u;
    uint8_t code[] = {0x75, 0x05}; /* jne 0x00401807 */
    const uint32_t target = code_base + sizeof(code) + 5;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->flags.zf = false;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == target);
    ASSERT(ctx->regs.x86.eip == target);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(x86_context_x87_defaults) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ASSERT(ctx->regs.x86.x87.control_word == 0x037f);
    ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);
    ASSERT(ctx->regs.x86.x87.top == 0);
    ASSERT(((ctx->regs.x86.x87.status_word >> 11) & 7) == 0);

    ASSERT(hb_x87_push_f64(&ctx->regs.x86.x87, 1.25) == HB_OK);
    ASSERT(ctx->regs.x86.x87.tag_word != 0xffff);
    ASSERT(hb_context_reset(ctx) == HB_OK);
    ASSERT(ctx->regs.x86.x87.control_word == 0x037f);
    ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);
    ASSERT(ctx->regs.x86.x87.top == 0);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(x87_fldcw_fnstcw_fistp_rounding_modes) {
    hb_x87_state_t x87;
    uint16_t cw = 0;
    int32_t out = 0;

    hb_x87_reset(&x87);
    ASSERT(hb_x87_fnstcw(&x87, &cw) == HB_OK);
    ASSERT(cw == 0x037f);

    ASSERT(hb_x87_fldcw(&x87, (uint16_t)((cw & ~(3u << 10)) | (3u << 10))) == HB_OK);
    ASSERT(hb_x87_push_f64(&x87, -3.9) == HB_OK);
    ASSERT(hb_x87_fistp_i32(&x87, &out) == HB_OK);
    ASSERT(out == -3);
    ASSERT(x87.tag_word == 0xffff);

    ASSERT(hb_x87_fldcw(&x87, (uint16_t)((cw & ~(3u << 10)) | (1u << 10))) == HB_OK);
    ASSERT(hb_x87_push_f64(&x87, -3.1) == HB_OK);
    ASSERT(hb_x87_fistp_i32(&x87, &out) == HB_OK);
    ASSERT(out == -4);

    ASSERT(hb_x87_fldcw(&x87, (uint16_t)((cw & ~(3u << 10)) | (2u << 10))) == HB_OK);
    ASSERT(hb_x87_push_f64(&x87, -3.9) == HB_OK);
    ASSERT(hb_x87_fistp_i32(&x87, &out) == HB_OK);
    ASSERT(out == -3);

    tests_passed++;
}

TEST(x87_environment_control_state_helpers) {
    hb_x87_state_t x87;
    const uint16_t dirty_status = (uint16_t)(0x80ffu | (3u << 11) | (1u << 14));

    hb_x87_reset(&x87);
    x87.control_word = 0x0f7f;
    x87.status_word = dirty_status;
    x87.tag_word = 0x5555;
    x87.top = 3;

    ASSERT(hb_x87_fnclex(&x87) == HB_OK);
    ASSERT(x87.control_word == 0x0f7f);
    ASSERT(x87.tag_word == 0x5555);
    ASSERT(x87.top == 3);
    ASSERT(x87.status_word == (uint16_t)((3u << 11) | (1u << 14)));

    ASSERT(hb_x87_fninit(&x87) == HB_OK);
    ASSERT(x87.control_word == 0x037f);
    ASSERT(x87.status_word == 0);
    ASSERT(x87.tag_word == 0xffff);
    ASSERT(x87.top == 0);

    ASSERT(hb_x87_push_f64(&x87, 2.75) == HB_OK);
    x87.control_word = (uint16_t)((x87.control_word & ~(3u << 10)) | (1u << 10));
    ASSERT(hb_x87_frndint(&x87) == HB_OK);
    {
        double rounded = 0.0;
        ASSERT(hb_x87_st_f64(&x87, 0, &rounded) == HB_OK);
        ASSERT(rounded == 2.0);
    }

    tests_passed++;
}

TEST(decode_interp_x86_x87_frndint_helper) {
    const uint32_t code_base = 0x00412000u;
    const uint32_t stack_base = 0x00210000u;
    const uint32_t esp = stack_base + 0x800u;
    const uint32_t ret_addr = 0x12345678u;
    uint8_t code[] = {
        0x8b, 0xff,             /* mov edi, edi */
        0x55,                   /* push ebp */
        0x8b, 0xec,             /* mov ebp, esp */
        0x83, 0xec, 0x08,       /* sub esp, 8 */
        0xdd, 0x45, 0x08,       /* fld qword [ebp+8] */
        0xd9, 0xfc,             /* frndint */
        0xdd, 0x5d, 0xf8,       /* fstp qword [ebp-8] */
        0xdd, 0x45, 0xf8,       /* fld qword [ebp-8] */
        0x8b, 0xe5,             /* mov esp, ebp */
        0x5d,                   /* pop ebp */
        0xc3                    /* ret */
    };
    hb_decoded_t d;
    ASSERT(hb_decode_x86(code + 11, sizeof(code) - 11, code_base + 11, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FRNDINT);
    ASSERT(d.len == 2);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(!func->has_unsupported);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, esp, ret_addr) == HB_OK);
    {
        double arg = 2.75;
        ASSERT(hb_memory_write(ctx->memory, esp + 4, &arg, sizeof(arg)) == HB_OK);
    }
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = esp;
    ctx->regs.x86.ebp = 0xabcdef00u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == ret_addr);
    ASSERT(ctx->regs.x86.eip == ret_addr);
    {
        double rounded = 0.0;
        ASSERT(hb_x87_st_f64(&ctx->regs.x86.x87, 0, &rounded) == HB_OK);
        ASSERT(rounded == 3.0);
    }

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_x87_environment_control_family) {
    hb_decoded_t d;
    uint8_t fwait[] = {0x9b};
    uint8_t fnclex[] = {0xdb, 0xe2};
    uint8_t fninit[] = {0xdb, 0xe3};
    uint8_t fclex[] = {0x9b, 0xdb, 0xe2};
    uint8_t finit[] = {0x9b, 0xdb, 0xe3};

    ASSERT(hb_decode_x86(fwait, sizeof(fwait), 0x00405000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);

    ASSERT(hb_decode_x86(fnclex, sizeof(fnclex), 0x00405010u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNCLEX);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x86(fninit, sizeof(fninit), 0x00405020u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNINIT);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x86(fclex, sizeof(fclex), 0x00405030u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);
    ASSERT(hb_decode_x86(fclex + d.len, sizeof(fclex) - d.len, 0x00405030u + d.len, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNCLEX);

    ASSERT(hb_decode_x86(finit, sizeof(finit), 0x00405040u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);
    ASSERT(hb_decode_x86(finit + d.len, sizeof(finit) - d.len, 0x00405040u + d.len, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNINIT);

    tests_passed++;
}

TEST(decode_x64_x87_environment_control_family) {
    hb_decoded_t d;
    uint8_t fwait[] = {0x9b};
    uint8_t fnclex[] = {0xdb, 0xe2};
    uint8_t fninit[] = {0xdb, 0xe3};
    uint8_t fclex[] = {0x9b, 0xdb, 0xe2};
    uint8_t finit[] = {0x9b, 0xdb, 0xe3};

    ASSERT(hb_decode_x64(fwait, sizeof(fwait), 0x140005000ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);

    ASSERT(hb_decode_x64(fnclex, sizeof(fnclex), 0x140005010ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNCLEX);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x64(fninit, sizeof(fninit), 0x140005020ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNINIT);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x64(fclex, sizeof(fclex), 0x140005030ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);
    ASSERT(hb_decode_x64(fclex + d.len, sizeof(fclex) - d.len, 0x140005030ULL + d.len, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNCLEX);

    ASSERT(hb_decode_x64(finit, sizeof(finit), 0x140005040ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);
    ASSERT(hb_decode_x64(finit + d.len, sizeof(finit) - d.len, 0x140005040ULL + d.len, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FNINIT);

    tests_passed++;
}

TEST(interp_x64_x87_environment_control_family) {
    uint8_t code[] = {0xdb, 0xe2, 0xdb, 0xe3};
    uint64_t base = (uint64_t)(uintptr_t)code;
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == base + sizeof(code));
    ASSERT(ctx->regs.x64.rip == base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_x87_load_store_control_conversion_core) {
    const uint32_t code_base = 0x00402000u;
    const uint32_t data_base = 0x00200000u;
    uint8_t code[] = {
        0xd9, 0x6b, 0x00, /* fldcw word [ebx] */
        0xdb, 0x43, 0x04, /* fild dword [ebx+4] */
        0xdb, 0x5b, 0x08, /* fistp dword [ebx+8] */
        0xdd, 0x43, 0x10, /* fld qword [ebx+0x10] */
        0xdd, 0x53, 0x28, /* fst qword [ebx+0x28] */
        0xdd, 0x5b, 0x18, /* fstp qword [ebx+0x18] */
        0xd9, 0x7b, 0x20, /* fnstcw word [ebx+0x20] */
        0xdf, 0xe0        /* fnstsw ax */
    };
    uint8_t fst_m64[] = {0xdd, 0x55, 0xf0}; /* fst qword [ebp-0x10] */
    uint16_t cw = 0x0f7f; /* default CW with round-toward-zero */
    int32_t input_i32 = -3;
    int32_t output_i32 = 0;
    double input_f64 = 12.5;
    double output_f64 = 0.0;
    double output_fst_f64 = 0.0;
    uint16_t stored_cw = 0;
    hb_decoded_t d;

    ASSERT(hb_decode_x86(fst_m64, sizeof(fst_m64), code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_X87_FST);
    ASSERT(d.op1.is_mem && d.op1.size == 8 && d.op1.mem.base == HB_REG_RBP && d.op1.mem.disp == -0x10);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base, &cw, sizeof(cw)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base + 4, &input_i32, sizeof(input_i32)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base + 0x10, &input_f64, sizeof(input_f64)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = data_base;

    hb_exec_result_t exec;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(hb_memory_read(ctx->memory, data_base + 8, &output_i32, sizeof(output_i32)) == HB_OK);
    ASSERT(output_i32 == -3);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x28, &output_fst_f64, sizeof(output_fst_f64)) == HB_OK);
    ASSERT(output_fst_f64 == input_f64);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x18, &output_f64, sizeof(output_f64)) == HB_OK);
    ASSERT(output_f64 == 12.5);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x20, &stored_cw, sizeof(stored_cw)) == HB_OK);
    ASSERT(stored_cw == cw);
    ASSERT((ctx->regs.x86.eax & 0xffffu) == ctx->regs.x86.x87.status_word);
    ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_x87_memory_arithmetic_compare_core) {
    const uint32_t code_base = 0x00403000u;
    const uint32_t data_base = 0x00201000u;
    uint8_t code[] = {
        0xdd, 0x43, 0x00, /* fld qword [ebx] */
        0xdc, 0x43, 0x08, /* fadd qword [ebx+8] */
        0xdc, 0x4b, 0x10, /* fmul qword [ebx+0x10] */
        0xdc, 0x63, 0x18, /* fsub qword [ebx+0x18] */
        0xdc, 0x73, 0x20, /* fdiv qword [ebx+0x20] */
        0xdd, 0x5b, 0x28, /* fstp qword [ebx+0x28] */
        0xdd, 0x43, 0x30, /* fld qword [ebx+0x30] */
        0xdc, 0x53, 0x30, /* fcom qword [ebx+0x30] */
        0xdf, 0xe0,       /* fnstsw ax */
        0xdc, 0x5b, 0x30  /* fcomp qword [ebx+0x30] */
    };
    double values[] = {2.0, 3.0, 4.0, 5.0, 3.0, 0.0, 7.0};
    double output = 0.0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base, values, sizeof(values)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = data_base;

    hb_exec_result_t exec;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x28, &output, sizeof(output)) == HB_OK);
    ASSERT(output == 5.0);
    ASSERT((ctx->regs.x86.eax & (1u << 14)) != 0); /* FCOM equal => C3 */
    ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_x87_stack_register_pop_core) {
    const uint32_t code_base = 0x00404000u;
    const uint32_t data_base = 0x00202000u;
    uint8_t code[] = {
        0xdd, 0x43, 0x00, /* fld qword [ebx]      ; st0=4 */
        0xdd, 0x43, 0x08, /* fld qword [ebx+8]    ; st0=2, st1=4 */
        0xd8, 0xc1,       /* fadd st0, st1        ; st0=6 */
        0xd9, 0xc9,       /* fxch st1             ; st0=4, st1=6 */
        0xde, 0xc1,       /* faddp st1, st0       ; st0=10 */
        0xdd, 0x5b, 0x10, /* fstp qword [ebx+0x10] */
        0xdd, 0x43, 0x18, /* fld qword [ebx+0x18] ; st0=7 */
        0xdd, 0x43, 0x18, /* fld qword [ebx+0x18] ; st0=7, st1=7 */
        0xde, 0xd9,       /* fcompp               ; equal, pop twice */
        0xdf, 0xe0        /* fnstsw ax */
    };
    double values[] = {4.0, 2.0, 0.0, 7.0};
    double output = 0.0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base, values, sizeof(values)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = data_base;

    hb_exec_result_t exec;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x10, &output, sizeof(output)) == HB_OK);
    ASSERT(output == 10.0);
    ASSERT((ctx->regs.x86.eax & (1u << 14)) != 0);
    ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_x87_environment_control_family) {
    const uint32_t code_base = 0x00405000u;
    const uint16_t dirty_status = (uint16_t)(0x80ffu | (3u << 11) | (1u << 14));
    struct {
        uint8_t code[3];
        size_t len;
        int resets;
    } cases[] = {
        {{0xdb, 0xe2, 0x00}, 2, 0},
        {{0x9b, 0xdb, 0xe2}, 3, 0},
        {{0xdb, 0xe3, 0x00}, 2, 1},
        {{0x9b, 0xdb, 0xe3}, 3, 1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, cases[i].code, cases[i].len, code_base);
        hb_ir_func_t* func = NULL;
        ASSERT(dec != NULL);
        ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
        hb_decoder_destroy(dec);
        ASSERT(func != NULL);

        hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
        ASSERT(ctx != NULL);
        ctx->memory = hb_memory_create(0);
        ASSERT(ctx->memory != NULL);
        ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                     HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
        ASSERT(hb_memory_write(ctx->memory, code_base, cases[i].code, cases[i].len) == HB_OK);

        ctx->pc = code_base;
        ctx->regs.x86.eip = code_base;
        ctx->regs.x86.x87.control_word = 0x0f7f;
        ctx->regs.x86.x87.status_word = dirty_status;
        ctx->regs.x86.x87.tag_word = 0x5555;
        ctx->regs.x86.x87.top = 3;

        hb_exec_result_t exec;
        ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &exec) == HB_OK);
        ASSERT(exec.result == HB_OK);
        ASSERT(ctx->pc == code_base + cases[i].len);
        ASSERT(ctx->regs.x86.eip == code_base + cases[i].len);

        if (cases[i].resets) {
            ASSERT(ctx->regs.x86.x87.control_word == 0x037f);
            ASSERT(ctx->regs.x86.x87.status_word == 0);
            ASSERT(ctx->regs.x86.x87.tag_word == 0xffff);
            ASSERT(ctx->regs.x86.x87.top == 0);
        } else {
            ASSERT(ctx->regs.x86.x87.control_word == 0x0f7f);
            ASSERT(ctx->regs.x86.x87.tag_word == 0x5555);
            ASSERT(ctx->regs.x86.x87.top == 3);
            ASSERT(ctx->regs.x86.x87.status_word == (uint16_t)((3u << 11) | (1u << 14)));
        }

        hb_context_destroy(ctx);
        hb_ir_func_destroy(func);
    }

    tests_passed++;
}

TEST(wow64cpu_process_thread_contract_spine) {
    hb_wow64_process_t process = {0};
    hb_wow64_thread_t thread = {0};
    const uint8_t* bop = NULL;
    uint32_t bop_size = 0;

    process.size = sizeof(process);
    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(process.version == HB_WOW64CPU_ABI_VERSION);
    ASSERT(process.memory != NULL);
    ASSERT(process.guest32_base != NULL);
    ASSERT(((uintptr_t)process.guest32_base & 0xffffffffULL) == 0);
    ASSERT(hb_wow64cpu_get_bop_code(&process, &bop, &bop_size) == HB_OK);
    ASSERT(bop_size == 2);
    ASSERT(bop[0] == 0x0f && bop[1] == 0xff);

    thread.size = sizeof(thread);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(thread.version == HB_WOW64CPU_ABI_VERSION);
    ASSERT(thread.machine == HB_WOW64_MACHINE_I386);
    ASSERT(thread.ctx != NULL);
    ASSERT(thread.ctx->arch == HB_ARCH_X86);
    ASSERT(thread.ctx->memory == process.memory);
    ASSERT(thread.ctx->regs.x86.x87.control_word == 0x037f);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_i386_context_roundtrip) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out = {.size = sizeof(out)};

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);

    hb_x87_reset(&in.x87);
    ASSERT(hb_x87_fldcw(&in.x87, (uint16_t)(0x037f | (3u << 10))) == HB_OK);
    in.eax = 1; in.ebx = 2; in.ecx = 3; in.edx = 4;
    in.esi = 5; in.edi = 6; in.esp = 0x70000000u; in.ebp = 0x70001000u;
    in.eip = 0x00401234u; in.eflags = 0x202u;
    in.fs_base = 0x7ffdf000u; in.gs_base = 0;

    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(thread.ctx->pc == in.eip);
    ASSERT(thread.ctx->fs_base == in.fs_base);
    ASSERT(thread.ctx->regs.x86.eax == 1);
    ASSERT(thread.ctx->regs.x86.x87.control_word == in.x87.control_word);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out) == HB_OK);
    ASSERT(out.version == HB_WOW64CPU_ABI_VERSION);
    ASSERT(out.eip == in.eip);
    ASSERT(out.esp == in.esp);
    ASSERT(out.fs_base == in.fs_base);
    ASSERT(out.x87.control_word == in.x87.control_word);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_memory_notify_updates_guest32_vma) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    const uint32_t guest = 0x01000000u;

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, guest, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    uint64_t gen = hb_memory_generation(process.memory);
    ASSERT(gen != 0);
    ASSERT(hb_memory_guest32_to_host(process.memory, guest + 0x10) != NULL);

    ASSERT(hb_wow64cpu_notify_memory_protect(&process, guest, 4096, HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_generation(process.memory) > gen);
    ASSERT(hb_memory_write_u32(process.memory, guest + 0x10, 0xfeedfaceu) == HB_ERR_MEMORY_FAULT);

    ASSERT(hb_wow64cpu_notify_memory_free(&process, guest, 4096) == HB_OK);
    ASSERT(hb_memory_guest32_to_host(process.memory, guest) == NULL);

    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_runs_i386_guest32_block) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint8_t code[] = {
        0xb8, 0x78, 0x56, 0x34, 0x12, /* mov eax, 0x12345678 */
    };

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(process.memory, code_va, code, sizeof(code)) == HB_OK);

    in.eip = code_va;
    in.esp = 0x0010fff0u;
    in.eflags = 0x202u;
    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(code), &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(!exec.faulted);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    ASSERT(out_ctx.eax == 0x12345678u);
    ASSERT(out_ctx.eip == code_va + sizeof(code));

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_chains_control_transfer_blocks) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint32_t stack_va = 0x00100000u;
    const uint32_t exit_va = code_va + 0x20u;
    const uint8_t code[] = {
        0xb8, 0x78, 0x56, 0x34, 0x12,       /* target: mov eax, 0x12345678 */
        0xc3,                                     /* ret */
        0xe8, 0xf5, 0xff, 0xff, 0xff,             /* caller: call target */
        0xbb, 0xef, 0xbe, 0xad, 0xde,             /* mov ebx, 0xdeadbeef */
        0xe9, 0x0b, 0x00, 0x00, 0x00,             /* jmp exit_va */
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90,
        0x0f, 0xff                                /* WOW64 syscall BOP boundary */
    };

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, stack_va, 0x10000,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(process.memory, code_va, code, sizeof(code)) == HB_OK);

    in.eip = code_va + 6;
    in.esp = stack_va + 0x8000;
    in.eflags = 0x202u;
    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(code), &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(!exec.faulted);
    ASSERT(exec.blocks_executed == 3);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    ASSERT(out_ctx.eax == 0x12345678u);
    ASSERT(out_ctx.ebx == 0xdeadbeefu);
    ASSERT(out_ctx.eip == exit_va);
    ASSERT(out_ctx.esp == in.esp);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_fastpaths_wine_x86_wcslen_pattern) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint32_t stack_va = 0x00100000u;
    const uint32_t str_va = 0x00200000u;
    const uint32_t ret_va = 0x00401000u;
    const uint8_t code[] = {
        0x66, 0x90,
        0x55,
        0x89, 0xe5,
        0xb8, 0xfe, 0xff, 0xff, 0xff,
        0x8b, 0x4d, 0x08,
        0x0f, 0x1f, 0x00,
        0x66, 0x83, 0x7c, 0x01, 0x02, 0x00,
        0x8d, 0x40, 0x02,
        0x75, 0xf5,
        0xd1, 0xf8,
        0x5d,
        0xc3
    };
    const uint16_t text[] = {'P', 'A', 'T', 'H', 0};

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, stack_va, 0x10000,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, str_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(process.memory, code_va, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(process.memory, str_va, text, sizeof(text)) == HB_OK);

    in.eip = code_va;
    in.esp = stack_va + 0x8000u;
    in.ebp = 0xfeedfaceu;
    in.eflags = 0x202u;
    ASSERT(hb_memory_write_u32(process.memory, in.esp, ret_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 4u, str_va) == HB_OK);
    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(code), &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(!exec.faulted);
    ASSERT(exec.blocks_executed == 1);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    ASSERT(out_ctx.eax == 4);
    ASSERT(out_ctx.ecx == str_va);
    ASSERT(out_ctx.ebp == in.ebp);
    ASSERT(out_ctx.esp == in.esp + 4u);
    ASSERT(out_ctx.eip == ret_va);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_fastpaths_wine_x86_rtl_query_environment_variable_u) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint32_t stack_va = 0x00100000u;
    const uint32_t data_va = 0x00200000u;
    const uint32_t teb_va = 0x00300000u;
    const uint32_t peb_va = 0x00301000u;
    const uint32_t params_va = 0x00302000u;
    const uint32_t ret_va = 0x00401000u;
    const uint32_t name_va = data_va + 0x100u;
    const uint32_t name_buf_va = data_va + 0x120u;
    const uint32_t value_va = data_va + 0x180u;
    const uint32_t value_buf_va = data_va + 0x1a0u;
    const uint32_t env_va = data_va + 0x300u;
    const uint8_t code[] = {
        0x66, 0x90,
        0x55,
        0x89, 0xe5,
        0x53,
        0x57,
        0x56,
        0x50,
        0x8b, 0x5d, 0x10,
        0x8b, 0x75, 0x0c,
        0x8b, 0x7d, 0x08
    };
    const uint16_t name_text[] = {'p', 'a', 't', 'h'};
    const uint16_t env_text[] = {
        'F', 'O', 'O', '=', 'b', 'a', 'r', 0,
        'P', 'A', 'T', 'H', '=', 'C', ':', '\\', 'b', 'i', 'n', 0,
        0
    };
    uint16_t copied[8] = {0};

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, stack_va, 0x10000,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, data_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, teb_va, 0x4000,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(process.memory, code_va, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(process.memory, name_buf_va, name_text, sizeof(name_text)) == HB_OK);
    ASSERT(hb_memory_write(process.memory, env_va, env_text, sizeof(env_text)) == HB_OK);
    ASSERT(hb_memory_write_u16(process.memory, name_va, sizeof(name_text)) == HB_OK);
    ASSERT(hb_memory_write_u16(process.memory, name_va + 2u, sizeof(name_text)) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, name_va + 4u, name_buf_va) == HB_OK);
    ASSERT(hb_memory_write_u16(process.memory, value_va, 0) == HB_OK);
    ASSERT(hb_memory_write_u16(process.memory, value_va + 2u, sizeof(copied)) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, value_va + 4u, value_buf_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, teb_va + 0x30u, peb_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, peb_va + 0x10u, params_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, params_va + 0x48u, env_va) == HB_OK);

    in.eip = code_va;
    in.esp = stack_va + 0x8000u;
    in.ebp = 0xfeedfaceu;
    in.eflags = 0x202u;
    in.fs_base = teb_va;
    ASSERT(hb_memory_write_u32(process.memory, in.esp, ret_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 4u, 0) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 8u, name_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 12u, value_va) == HB_OK);
    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(code), &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(!exec.faulted);
    ASSERT(exec.blocks_executed == 1);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    ASSERT(out_ctx.eax == 0);
    ASSERT(out_ctx.ebp == in.ebp);
    ASSERT(out_ctx.esp == in.esp + 16u);
    ASSERT(out_ctx.eip == ret_va);
    ASSERT(hb_memory_read_u16(process.memory, value_va, &copied[0]) == HB_OK);
    ASSERT(copied[0] == 12);
    ASSERT(hb_memory_read(process.memory, value_buf_va, copied, sizeof(copied)) == HB_OK);
    ASSERT(copied[0] == 'C');
    ASSERT(copied[1] == ':');
    ASSERT(copied[2] == '\\');
    ASSERT(copied[3] == 'b');
    ASSERT(copied[4] == 'i');
    ASSERT(copied[5] == 'n');
    ASSERT(copied[6] == 0);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_fastpaths_wine_x86_strcmp_pattern) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint32_t stack_va = 0x00100000u;
    const uint32_t data_va = 0x00200000u;
    const uint32_t lhs_va = data_va + 0x100u;
    const uint32_t rhs_va = data_va + 0x120u;
    const uint32_t ret_va = 0x00401000u;
    const uint8_t code[] = {
        0x66, 0x90,
        0x55,
        0x89, 0xe5,
        0x8b, 0x45, 0x0c,
        0x8b, 0x55, 0x08,
        0x0f, 0xb6, 0x0a
    };
    const char lhs[] = "GetModuleHandleW";
    const char rhs[] = "GetModuleHandleA";

    ASSERT(hb_wow64cpu_process_init(&process) == HB_OK);
    ASSERT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, stack_va, 0x10000,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_wow64cpu_notify_memory_alloc(&process, data_va, 4096,
                                           HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(process.memory, code_va, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(process.memory, lhs_va, lhs, sizeof(lhs)) == HB_OK);
    ASSERT(hb_memory_write(process.memory, rhs_va, rhs, sizeof(rhs)) == HB_OK);

    in.eip = code_va;
    in.esp = stack_va + 0x8000u;
    in.ebp = 0xfeedfaceu;
    in.eflags = 0x202u;
    ASSERT(hb_memory_write_u32(process.memory, in.esp, ret_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 4u, lhs_va) == HB_OK);
    ASSERT(hb_memory_write_u32(process.memory, in.esp + 8u, rhs_va) == HB_OK);
    ASSERT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    ASSERT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(code), &exec) == HB_OK);
    ASSERT(exec.result == HB_OK);
    ASSERT(!exec.faulted);
    ASSERT(exec.blocks_executed == 1);

    ASSERT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    ASSERT(out_ctx.eax == 1);
    ASSERT(out_ctx.ebp == in.ebp);
    ASSERT(out_ctx.esp == in.esp + 4u);
    ASSERT(out_ctx.eip == ret_va);

    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    tests_passed++;
}

TEST(wow64cpu_simulate_honors_env_block_limit) {
    hb_wow64_process_t process = {.size = sizeof(process)};
    hb_wow64_thread_t thread = {.size = sizeof(thread)};
    hb_wow64_i386_context_t in = {.size = sizeof(in), .version = HB_WOW64CPU_ABI_VERSION};
    hb_wow64_i386_context_t out_ctx = {.size = sizeof(out_ctx)};
    hb_exec_result_t exec = {0};
    const uint32_t code_va = 0x00400000u;
    const uint32_t target_va = code_va + 0x100u;
    const uint8_t jmp_to_target[] = {
        0xe9, 0xfb, 0x00, 0x00, 0x00, /* jmp +0x100 */
    };
    const uint8_t jmp_to_start[] = {
        0xe9, 0xfb, 0xfe, 0xff, 0xff, /* jmp -0x105 */
    };
    char* saved_limit = save_env_var("MACRUNNER_HB_WOW64_BLOCK_LIMIT");
    int failed_line = 0;
    const char* failed_expr = NULL;

#define CHECK_WOW64_LIMIT(cond) do { if (!(cond)) { failed_line = __LINE__; failed_expr = #cond; goto cleanup; } } while (0)

    setenv("MACRUNNER_HB_WOW64_BLOCK_LIMIT", "4", 1);
    CHECK_WOW64_LIMIT(hb_wow64cpu_process_init(&process) == HB_OK);
    CHECK_WOW64_LIMIT(hb_wow64cpu_thread_init(&process, &thread) == HB_OK);
    CHECK_WOW64_LIMIT(hb_wow64cpu_notify_memory_alloc(&process, code_va, 4096,
                                                      HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    CHECK_WOW64_LIMIT(hb_memory_write(process.memory, code_va, jmp_to_target, sizeof(jmp_to_target)) == HB_OK);
    CHECK_WOW64_LIMIT(hb_memory_write(process.memory, target_va, jmp_to_start, sizeof(jmp_to_start)) == HB_OK);

    in.eip = code_va;
    in.esp = 0x0010fff0u;
    in.eflags = 0x202u;
    CHECK_WOW64_LIMIT(hb_wow64cpu_import_i386_context(&thread, &in) == HB_OK);
    CHECK_WOW64_LIMIT(hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, sizeof(jmp_to_target), &exec) == HB_OK);
    CHECK_WOW64_LIMIT(exec.result == HB_OK);
    CHECK_WOW64_LIMIT(!exec.faulted);
    CHECK_WOW64_LIMIT(exec.blocks_executed == 4);
    CHECK_WOW64_LIMIT(hb_wow64cpu_export_i386_context(&thread, &out_ctx) == HB_OK);
    CHECK_WOW64_LIMIT(out_ctx.eip == code_va);

cleanup:
    hb_wow64cpu_thread_destroy(&thread);
    hb_wow64cpu_process_destroy(&process);
    restore_env_var("MACRUNNER_HB_WOW64_BLOCK_LIMIT", saved_limit);
    if (failed_line) {
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, failed_line, failed_expr);
        tests_failed++;
        return;
    }
    tests_passed++;

#undef CHECK_WOW64_LIMIT
}

TEST(x86_abi_calling_convention_stack_contracts) {
    const uint32_t stack_base = 0x00100000u;
    const uint32_t stack_top = 0x00110000u;
    const uint32_t target = 0x00405000u;
    uint32_t value = 0;
    uint32_t out = 0xffffffffu;

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, stack_top - stack_base,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    uint32_t cdecl_args[] = {0x11u, 0x22u, 0x33u};
    hb_abi_x86_cdecl_t cdecl_call = {0};
    cdecl_call.stack_args = cdecl_args;
    cdecl_call.stack_arg_count = 3;
    ctx->regs.x86.esp = stack_top;
    ASSERT(hb_abi_x86_cdecl_call(ctx, target, &cdecl_call, &out) == HB_OK);
    ASSERT(out == 0);
    ASSERT(ctx->pc == target);
    ASSERT(ctx->regs.x86.eip == target);
    ASSERT(ctx->regs.x86.esp == stack_top - 16);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &value) == HB_OK);
    ASSERT(value == 0xffff0000u);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 4, &value) == HB_OK);
    ASSERT(value == 0x11u);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 12, &value) == HB_OK);
    ASSERT(value == 0x33u);

    uint32_t fast_stack_args[] = {0x3333u, 0x4444u};
    hb_abi_x86_fastcall_t fast_call = {0};
    fast_call.ecx = 0x1111u;
    fast_call.edx = 0x2222u;
    fast_call.stack_args = fast_stack_args;
    fast_call.stack_arg_count = 2;
    ctx->regs.x86.esp = stack_top;
    ASSERT(hb_abi_x86_fastcall_call(ctx, target + 0x10, &fast_call, NULL) == HB_OK);
    ASSERT(ctx->regs.x86.ecx == 0x1111u);
    ASSERT(ctx->regs.x86.edx == 0x2222u);
    ASSERT(ctx->regs.x86.esp == stack_top - 12);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 4, &value) == HB_OK);
    ASSERT(value == 0x3333u);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 8, &value) == HB_OK);
    ASSERT(value == 0x4444u);

    uint32_t this_args[] = {0x55u};
    hb_abi_x86_thiscall_t this_call = {0};
    this_call.this_ptr = 0x12345678u;
    this_call.stack_args = this_args;
    this_call.stack_arg_count = 1;
    ctx->regs.x86.esp = stack_top;
    ASSERT(hb_abi_x86_thiscall_call(ctx, target + 0x20, &this_call, NULL) == HB_OK);
    ASSERT(ctx->regs.x86.ecx == 0x12345678u);
    ASSERT(ctx->regs.x86.esp == stack_top - 8);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 4, &value) == HB_OK);
    ASSERT(value == 0x55u);

    hb_abi_x86_stdcall_t stdcall = {0};
    stdcall.stack_args = cdecl_args;
    stdcall.stack_arg_count = 1;
    ctx->regs.x86.esp = 0x20u;
    ctx->pc = 0x7777u;
    ASSERT(hb_abi_x86_stdcall_call(ctx, target, &stdcall, NULL) == HB_ERR_MEMORY_FAULT);
    ASSERT(ctx->pc == 0x7777u);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(decode_push_pop) {
    uint8_t code[] = {0x50, 0x58}; /* push rax; pop rax */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, 1, 0x1000, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSH);
    r = hb_decode_x64(code + 1, 1, 0x1001, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_POP);
    tests_passed++;
}

TEST(decode_mov_r8w_imm16_rex_operand_override) {
    uint8_t code[] = {
        0x66, 0x41, 0xb8, 0x04, 0x01,       /* mov r8w, 0x104 */
        0xff, 0x15, 0x7d, 0x4a, 0x00, 0x00  /* call qword ptr [rip+0x4a7d] */
    };
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140004358, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_R8);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0x104);
    ASSERT(d.op2.size == 2);

    r = hb_decode_x64(code + d.len, sizeof(code) - d.len, 0x14000435d, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_CALL);
    ASSERT(d.len == 6);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.mem.rip_relative);
    ASSERT(d.op1.mem.rip_target == 0x140004363ULL);
    ASSERT(d.op1.mem.disp == 0x4a7d);
    tests_passed++;
}

TEST(decode_group83_or_ecx_imm8) {
    uint8_t code[] = {0x83, 0xc9, 0x02}; /* or ecx, 2 */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140005523, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_OR);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 2);
    ASSERT(d.op2.size == 4);
    tests_passed++;
}

TEST(decode_div_r32_group_f7) {
    uint8_t code[] = {0xf7, 0xf6}; /* div esi */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x10002013f, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_DIV);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RSI);
    ASSERT(d.op1.size == 4);
    tests_passed++;
}

TEST(decode_idiv_r32_group_f7) {
    uint8_t code[] = {0xf7, 0xfe}; /* idiv esi */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140001000, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_IDIV);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RSI);
    ASSERT(d.op1.size == 4);
    tests_passed++;
}

TEST(decode_xadd_lock_r32_calc_atomic) {
    uint8_t code[] = {0xf0, 0x0f, 0xc1, 0x0b}; /* lock xadd %ecx,(%rbx) */
    uint8_t cmpxchg8b[] = {0x0f, 0xc7, 0x0b};  /* cmpxchg8b (%rbx) */
    uint8_t cmpxchg16b[] = {0xf0, 0x48, 0x0f, 0xc7, 0x4e, 0x40}; /* lock cmpxchg16b 0x40(%rsi) */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x100022309, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_XADD);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_reg);
    ASSERT(d.op2.reg == HB_REG_RCX);
    ASSERT(d.op2.size == 4);

    r = hb_decode_x64(cmpxchg8b, sizeof(cmpxchg8b), 0x100022400, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_CMPXCHG8B);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.size == 8);

    r = hb_decode_x64(cmpxchg16b, sizeof(cmpxchg16b), 0x87efd67b400ULL, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_CMPXCHG8B);
    ASSERT(d.len == 6);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.mem.base == HB_REG_RSI);
    ASSERT(d.op1.mem.disp == 0x40);
    ASSERT(d.op1.size == 16);
    tests_passed++;
}

TEST(interp_x64_lock_xadd_r32_memory) {
    struct {
        uint8_t code[4];
        uint32_t cell;
    } block;
    memset(&block, 0xcc, sizeof(block));
    block.code[0] = 0xf0; /* lock xadd %ecx,(%rbx) */
    block.code[1] = 0x0f;
    block.code[2] = 0xc1;
    block.code[3] = 0x0b;
    block.cell = 5;
    uint64_t base = (uint64_t)(uintptr_t)block.code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, sizeof(block.code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&block.cell;
    ctx->regs.x64.rcx = 7;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(block.cell == 12);
    ASSERT(ctx->regs.x64.rcx == 5);
    bool zf = true;
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    zf = ctx->flags.zf;
    ASSERT(!zf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_cmpxchg8b_cmpxchg16b_family) {
    uint8_t cmpxchg8b_code[] = {0x0f, 0xc7, 0x0e}; /* cmpxchg8b (%rsi) */
    uint8_t cmpxchg16b_code[] = {0xf0, 0x48, 0x0f, 0xc7, 0x4e, 0x40}; /* lock cmpxchg16b 0x40(%rsi) */
    uint64_t mem64 = 0;
    uint64_t mem128_area[10] = {0};
    uint64_t base8 = (uint64_t)(uintptr_t)cmpxchg8b_code;
    uint64_t base16 = (uint64_t)(uintptr_t)cmpxchg16b_code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, cmpxchg8b_code, sizeof(cmpxchg8b_code), base8);
    hb_ir_func_t* func8 = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func8) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func8 != NULL);

    dec = hb_decoder_create(HB_ARCH_X64, cmpxchg16b_code, sizeof(cmpxchg16b_code), base16);
    hb_ir_func_t* func16 = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func16) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func16 != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base8, sizeof(cmpxchg8b_code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, base16, sizeof(cmpxchg16b_code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&mem64, sizeof(mem64), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem128_area, sizeof(mem128_area), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    hb_exec_result_t out;
    mem64 = 0x5566778811223344ULL;
    ctx->pc = base8;
    ctx->regs.x64.rsi = (uint64_t)(uintptr_t)&mem64;
    ctx->regs.x64.rax = 0x11223344;
    ctx->regs.x64.rdx = 0x55667788;
    ctx->regs.x64.rbx = 0x11223344;
    ctx->regs.x64.rcx = 0xaabbccdd;
    ASSERT(hb_runtime_run(ctx, func8, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.zf == true);
    ASSERT(mem64 == 0xaabbccdd11223344ULL);

    mem64 = 0x0123456789abcdefULL;
    ctx->pc = base8;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rdx = 0;
    ASSERT(hb_runtime_run(ctx, func8, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.zf == false);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x89abcdefU);
    ASSERT((uint32_t)ctx->regs.x64.rdx == 0x01234567U);

    uint64_t* mem128 = &mem128_area[8];
    mem128[0] = 0x0102030405060708ULL;
    mem128[1] = 0x1112131415161718ULL;
    ctx->pc = base16;
    ctx->regs.x64.rsi = (uint64_t)(uintptr_t)mem128_area;
    ctx->regs.x64.rax = mem128[0];
    ctx->regs.x64.rdx = mem128[1];
    ctx->regs.x64.rbx = 0x2122232425262728ULL;
    ctx->regs.x64.rcx = 0x3132333435363738ULL;
    ASSERT(hb_runtime_run(ctx, func16, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.zf == true);
    ASSERT(mem128[0] == 0x2122232425262728ULL);
    ASSERT(mem128[1] == 0x3132333435363738ULL);

    mem128[0] = 0x4142434445464748ULL;
    mem128[1] = 0x5152535455565758ULL;
    ctx->pc = base16;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rdx = 0;
    ASSERT(hb_runtime_run(ctx, func16, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->regs.x64.rax == 0x4142434445464748ULL);
    ASSERT(ctx->regs.x64.rdx == 0x5152535455565758ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func8);
    hb_ir_func_destroy(func16);
    tests_passed++;
}

TEST(interp_x64_div_r32_calc_startup) {
    /* Win7 Calc startup executes: EDX:EAX = 0x00000000_80000000; div esi (10). */
    uint8_t code[] = {0xf7, 0xf6}; /* div esi */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x80000000ULL;
    ctx->regs.x64.rdx = 0;
    ctx->regs.x64.rsi = 10;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 214748364ULL);
    ASSERT(ctx->regs.x64.rdx == 8ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_div_by_zero) {
    uint8_t code[] = {0xf7, 0xf6}; /* div esi */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 42;
    ctx->regs.x64.rdx = 0;
    ctx->regs.x64.rsi = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_EXEC_FAULT);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_div_overflow) {
    uint8_t code[] = {0xf7, 0xf6}; /* div esi */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rdx = 1;
    ctx->regs.x64.rsi = 1;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_EXEC_FAULT);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_idiv_signed) {
    uint8_t code[] = {0xf7, 0xfe}; /* idiv esi */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xffffff9cULL; /* low half of -100 */
    ctx->regs.x64.rdx = 0xffffffffULL; /* high half of -100 */
    ctx->regs.x64.rsi = 7;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0xfffffff2U); /* -14 */
    ASSERT((uint32_t)ctx->regs.x64.rdx == 0xfffffffeU); /* -2 */

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_jcc_near_taken) {
    uint8_t code[] = {
        0x83, 0xf8, 0x07,                   /* cmp eax, 7 */
        0x0f, 0x84, 0x10, 0x00, 0x00, 0x00  /* je +0x10 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 7;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == base + sizeof(code) + 0x10);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_jcc_near_not_taken) {
    uint8_t code[] = {
        0x83, 0xf8, 0x07,                   /* cmp eax, 7 */
        0x0f, 0x84, 0x10, 0x00, 0x00, 0x00  /* je +0x10 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x64;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_jcc_near_backward) {
    uint8_t code[] = {
        0x83, 0xf8, 0x07,                   /* cmp eax, 7 */
        0x0f, 0x84, 0xf6, 0xff, 0xff, 0xff  /* je -10 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 7;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == base - 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(lift_x64_calc_near_jcc_stops_at_branch) {
    uint8_t code[] = {
        0x0f, 0x84, 0x2a, 0x90, 0xfe, 0xff, /* je 0x10000e2e6 */
        0x3d, 0x00, 0x02, 0x00, 0x00,       /* cmp eax, 0x200 */
        0x0f, 0x84, 0x1f, 0x90, 0xfe, 0xff  /* je 0x10000e2e6 */
    };
    uint64_t base = 0x1000252b6ULL;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg != NULL);
    ASSERT(func->cfg->entry != NULL);
    ASSERT(func->cfg->entry->instr_count == 1);
    ASSERT(func->cfg->entry->instrs[0].op == HB_IR_Jcc);
    ASSERT(func->cfg->entry->instrs[0].guest_addr == base);
    ASSERT(func->cfg->entry->instrs[0].guest_len == 6);
    ASSERT(func->cfg->entry->instrs[0].target == 0x10000e2e6ULL);

    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_x86_loop_jrcxz_family) {
    uint64_t base = 0x1000;
    struct {
        hb_arch_t arch;
        uint8_t code[3];
        size_t len;
        int opcode;
        uint8_t count_size;
        int kind;
        uint64_t target;
    } cases[] = {
        {HB_ARCH_X64, {0xe0, 0x05, 0x00}, 2, HB_INS_LOOP, 8, 0, 0x1007},
        {HB_ARCH_X64, {0xe1, 0x05, 0x00}, 2, HB_INS_LOOP, 8, 1, 0x1007},
        {HB_ARCH_X64, {0xe2, 0x05, 0x00}, 2, HB_INS_LOOP, 8, 2, 0x1007},
        {HB_ARCH_X64, {0xe3, 0x05, 0x00}, 2, HB_INS_JRCXZ, 8, 3, 0x1007},
        {HB_ARCH_X64, {0x67, 0xe3, 0x05}, 3, HB_INS_JRCXZ, 4, 3, 0x1008},
        {HB_ARCH_X86, {0xe2, 0x05, 0x00}, 2, HB_INS_LOOP, 4, 2, 0x1007},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        hb_result_t r = cases[i].arch == HB_ARCH_X64 ?
            hb_decode_x64(cases[i].code, cases[i].len, base, &d) :
            hb_decode_x86(cases[i].code, cases[i].len, base, &d);
        if (r != HB_OK) fprintf(stderr, "FAIL: loop decode case=%zu arch=%d r=%d first=%02x len=%u opcode=%d\n",
                                i, cases[i].arch, r, cases[i].code[0], d.len, (int)d.opcode);
        ASSERT(r == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.is_branch && d.is_conditional);
        ASSERT(d.branch_target == cases[i].target);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == cases[i].count_size);
        ASSERT(d.op2.is_imm && d.op2.imm == cases[i].kind);
    }
    tests_passed++;
}

TEST(interp_x64_loop_jrcxz_family) {
    uint8_t jrcxz[] = {0xe3, 0x05};
    uint8_t loop[] = {0xe2, 0x05};
    uint8_t loopne[] = {0xe0, 0x05};
    uint8_t loope[] = {0xe1, 0x05};
    uint64_t base = 0x2000;
    struct {
        uint8_t* code;
        size_t len;
        uint64_t rcx_in;
        bool zf;
        uint64_t pc_out;
        uint64_t rcx_out;
    } cases[] = {
        {jrcxz, sizeof(jrcxz), 0, false, base + 7, 0},
        {jrcxz, sizeof(jrcxz), 1, false, base + 2, 1},
        {loop, sizeof(loop), 2, false, base + 7, 1},
        {loop, sizeof(loop), 1, false, base + 2, 0},
        {loopne, sizeof(loopne), 2, false, base + 7, 1},
        {loopne, sizeof(loopne), 2, true, base + 2, 1},
        {loope, sizeof(loope), 2, true, base + 7, 1},
        {loope, sizeof(loope), 2, false, base + 2, 1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, cases[i].code, cases[i].len, base);
        hb_ir_func_t* func = NULL;
        hb_context_t* ctx = NULL;
        hb_exec_result_t out;
        ASSERT(dec != NULL);
        ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
        hb_decoder_destroy(dec);
        ASSERT(func != NULL);
        ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
        ASSERT(ctx != NULL);
        ctx->pc = base;
        ctx->regs.x64.rip = base;
        ctx->regs.x64.rcx = cases[i].rcx_in;
        ctx->flags.zf = cases[i].zf;
        ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
        ASSERT(out.result == HB_OK);
        ASSERT(!out.faulted);
        ASSERT(ctx->pc == cases[i].pc_out);
        ASSERT(ctx->regs.x64.rcx == cases[i].rcx_out);
        ASSERT(ctx->flags.zf == cases[i].zf);
        hb_context_destroy(ctx);
        hb_ir_func_destroy(func);
    }
    tests_passed++;
}

TEST(jit_x64_loop_jrcxz_family) {
    hb_ir_func_t* func = hb_ir_func_create(0x3000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3000);
    hb_ir_block_t* fallthrough = hb_ir_block_create(1, 0x3002);
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_context_t* ctx = NULL;
    hb_exec_result_t out;
    hb_ir_instr_t* i;
    hb_ir_cfg_add_block(func->cfg, blk);
    hb_ir_cfg_add_block(func->cfg, fallthrough);
    func->cfg->entry = blk;
    hb_ir_builder_set_block(b, blk);
    i = hb_ir_emit(b, HB_IR_LOOP);
    ASSERT(i != NULL);
    i->dst = hb_ir_reg(HB_REG_RCX, HB_SIZE_64);
    i->src1 = hb_ir_imm(2, HB_SIZE_8);
    i->guest_addr = 0x3000;
    i->guest_len = 2;
    i->target = 0x3010;
    hb_ir_builder_destroy(b);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3000;
    ctx->regs.x64.rip = 0x3000;
    ctx->regs.x64.rcx = 1;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    if (out.result != HB_OK) fprintf(stderr, "FAIL: loop jit result=%d faulted=%d reason=%s\n",
                                     out.result, out.faulted ? 1 : 0,
                                     out.fault_reason ? out.fault_reason : "");
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->pc == 0x3002);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_lea_r9_rsp_disp8_rex_r) {
    uint8_t code[] = {0x4c, 0x8d, 0x4c, 0x24, 0x44}; /* lea r9, [rsp+0x44] */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000252d7, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_LEA);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_R9);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_mem);
    ASSERT(d.op2.mem.base == HB_REG_RSP);
    ASSERT(d.op2.mem.disp == 0x44);
    ASSERT(d.op2.size == 8);
    tests_passed++;
}

TEST(decode_btr_r32_imm8_group_0fba) {
    uint8_t code[] = {0x0f, 0xba, 0xf0, 0x1f}; /* btr eax, 31 */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000052d7, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_BTR);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 31);
    tests_passed++;
}

TEST(interp_x64_btr_r32_imm8_calc_sign_bit) {
    uint8_t code[] = {0x0f, 0xba, 0xf0, 0x1f}; /* btr eax, 31 */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x80000064ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x64U);
    ASSERT(ctx->flags.cf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_bts_r64_reg_notepadpp) {
    uint8_t code[] = {0x48, 0x0f, 0xab, 0xc8}; /* bts rax, rcx */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140001910, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_BTS);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_reg);
    ASSERT(d.op2.reg == HB_REG_RCX);
    ASSERT(d.op2.size == 8);
    tests_passed++;
}

TEST(interp_x64_bts_r64_reg_notepadpp) {
    uint8_t code[] = {0x48, 0x0f, 0xab, 0xc8}; /* bts rax, rcx */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = 8;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x100);
    ASSERT(ctx->flags.cf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_bit_test_reg_family) {
    struct {
        uint8_t code[3];
        int opcode;
        int src;
    } cases[] = {
        {{0x0f, 0xa3, 0xd0}, HB_INS_BT,  HB_REG_RDX}, /* bt  eax, edx */
        {{0x0f, 0xab, 0xc8}, HB_INS_BTS, HB_REG_RCX}, /* bts eax, ecx */
        {{0x0f, 0xb3, 0xf0}, HB_INS_BTR, HB_REG_RSI}, /* btr eax, esi */
        {{0x0f, 0xbb, 0xf8}, HB_INS_BTC, HB_REG_RDI}, /* btc eax, edi */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x86(cases[i].code, sizeof(cases[i].code), 0x00405000u, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == 3);
        ASSERT(d.op1.is_reg);
        ASSERT(d.op1.reg == HB_REG_RAX);
        ASSERT(d.op1.size == 4);
        ASSERT(d.op2.is_reg);
        ASSERT(d.op2.reg == cases[i].src);
        ASSERT(d.op2.size == 4);
    }

    tests_passed++;
}

TEST(decode_x86_bit_test_imm_group_0fba) {
    struct {
        uint8_t code[4];
        int opcode;
    } cases[] = {
        {{0x0f, 0xba, 0xe0, 0x05}, HB_INS_BT},  /* bt  eax, 5 */
        {{0x0f, 0xba, 0xe8, 0x05}, HB_INS_BTS}, /* bts eax, 5 */
        {{0x0f, 0xba, 0xf0, 0x05}, HB_INS_BTR}, /* btr eax, 5 */
        {{0x0f, 0xba, 0xf8, 0x05}, HB_INS_BTC}, /* btc eax, 5 */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x86(cases[i].code, sizeof(cases[i].code), 0x00406000u, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == 4);
        ASSERT(d.op1.is_reg);
        ASSERT(d.op1.reg == HB_REG_RAX);
        ASSERT(d.op1.size == 4);
        ASSERT(d.op2.is_imm);
        ASSERT(d.op2.imm == 5);
    }

    tests_passed++;
}

TEST(interp_x86_bt_and_btc_family) {
    const uint32_t code_base = 0x00407000u;
    uint8_t code[] = {
        0x0f, 0xa3, 0xd0,       /* bt  eax, edx */
        0x0f, 0xba, 0xf8, 0x01  /* btc eax, 1 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x8;
    ctx->regs.x86.edx = 3;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0xa);
    ASSERT(ctx->flags.cf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_bit_scan_family) {
    hb_decoded_t d;
    uint8_t bsr_trigger[] = {0x0f, 0xbd, 0xd7};     /* bsr edi, edx */
    uint8_t bsf_sibling[] = {0x0f, 0xbc, 0xc8};     /* bsf eax, ecx */
    uint8_t tzcnt_trigger[] = {0xf3, 0x0f, 0xbc, 0xc8}; /* tzcnt eax, ecx */
    uint8_t lzcnt_sibling[] = {0xf3, 0x0f, 0xbd, 0xc3}; /* lzcnt ebx, eax */
    uint8_t bsr_mem[] = {0x0f, 0xbd, 0x50, 0x04};   /* bsr [eax+4], edx */
    uint8_t bsr_r16[] = {0x66, 0x0f, 0xbd, 0xd7};   /* bsr di, dx */

    ASSERT(hb_decode_x86(bsr_trigger, sizeof(bsr_trigger), 0x7bd81900u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSR);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDI && d.op2.size == 4);

    ASSERT(hb_decode_x86(bsf_sibling, sizeof(bsf_sibling), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSF);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 4);

    ASSERT(hb_decode_x86(tzcnt_trigger, sizeof(tzcnt_trigger), 0x7bdb2ce5u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TZCNT);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 4);

    ASSERT(hb_decode_x86(lzcnt_sibling, sizeof(lzcnt_sibling), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LZCNT);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RBX && d.op2.size == 4);

    ASSERT(hb_decode_x86(bsr_mem, sizeof(bsr_mem), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSR);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RAX && d.op2.mem.disp == 4);

    ASSERT(hb_decode_x86(bsr_r16, sizeof(bsr_r16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSR);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDI && d.op2.size == 2);

    tests_passed++;
}

TEST(interp_x86_bit_scan_family) {
    const uint32_t code_base = 0x00407500u;
    uint8_t code[] = {
        0x0f, 0xbd, 0xd7, /* bsr edi, edx */
        0x0f, 0xbc, 0xc8, /* bsf eax, ecx */
        0xf3, 0x0f, 0xbc, 0xc8, /* tzcnt eax, ecx */
        0xf3, 0x0f, 0xbd, 0xc3  /* lzcnt ebx, eax */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x00100020u;
    ctx->regs.x86.ebx = 0;
    ctx->regs.x86.edi = 0x00001f70u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 32u);
    ASSERT(ctx->regs.x86.edx == 12u);
    ASSERT(ctx->regs.x86.ecx == 5u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->flags.cf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_xmm_logical_family) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
    } cases[] = {
        {{0x0f, 0x54, 0xc1, 0x00}, 3, HB_INS_XMM_AND},  /* andps xmm0, xmm1 */
        {{0x0f, 0x55, 0xc1, 0x00}, 3, HB_INS_XMM_ANDN}, /* andnps xmm0, xmm1 */
        {{0x0f, 0x56, 0xc1, 0x00}, 3, HB_INS_XMM_OR},   /* orps xmm0, xmm1 */
        {{0x0f, 0x57, 0xc0, 0x00}, 3, HB_INS_XORPS},    /* xorps xmm0, xmm0 */
        {{0x66, 0x0f, 0xef, 0xc1}, 4, HB_INS_PXOR},     /* pxor xmm0, xmm1 */
        {{0x66, 0x0f, 0xdb, 0xc1}, 4, HB_INS_XMM_AND},  /* pand xmm0, xmm1 */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x86(cases[i].code, cases[i].len, 0x00408000u, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.is_reg);
        ASSERT(d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op1.size == 16);
        ASSERT(d.op2.is_reg);
        ASSERT(d.op2.size == 16);
    }

    tests_passed++;
}

TEST(interp_x86_xorps_zeroes_xmm0) {
    const uint32_t code_base = 0x00409000u;
    uint8_t code[] = {0x0f, 0x57, 0xc0}; /* xorps xmm0, xmm0 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x86.xmm[0][1] = 0x8877665544332211ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_mov_packed_family) {
    hb_decoded_t d;
    uint8_t movups_load[] = {0x0f, 0x10, 0xc1};          /* movups xmm0, xmm1 */
    uint8_t movups_store[] = {0x0f, 0x11, 0x44, 0x24, 0x04}; /* movups [esp+4], xmm0 */
    uint8_t movaps_load[] = {0x0f, 0x28, 0xc1};          /* movaps xmm0, xmm1 */
    uint8_t movaps_store[] = {0x0f, 0x29, 0xc1};         /* movaps xmm1, xmm0 */
    uint8_t movdqa_load[] = {0x66, 0x0f, 0x6f, 0xc1};    /* movdqa xmm0, xmm1 */

    ASSERT(hb_decode_x86(movups_load, sizeof(movups_load), 0x0040a000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movups_store, sizeof(movups_store), 0x0040a000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 16);
    ASSERT(d.op1.mem.base == HB_REG_RSP);
    ASSERT(d.op1.mem.disp == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movaps_load, sizeof(movaps_load), 0x0040a000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movaps_store, sizeof(movaps_store), 0x0040a000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movdqa_load, sizeof(movdqa_load), 0x0040a000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    tests_passed++;
}

TEST(interp_x86_xorps_movups_stack_store) {
    const uint32_t code_base = 0x0040b000u;
    const uint32_t stack_top = 0x00640000u;
    const uint32_t store_addr = stack_top - 0x18 + 4;
    uint8_t code[] = {
        0x83, 0xec, 0x18,       /* sub esp, 0x18 */
        0x0f, 0x57, 0xc0,       /* xorps xmm0, xmm0 */
        0x0f, 0x11, 0x44, 0x24, 0x04  /* movups [esp+4], xmm0 */
    };
    uint64_t lane0 = 1, lane1 = 1;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_top - 4096, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = stack_top;
    ctx->regs.x86.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x86.xmm[0][1] = 0x8877665544332211ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.esp == stack_top - 0x18);
    ASSERT(hb_memory_read_u64(ctx->memory, store_addr, &lane0) == HB_OK);
    ASSERT(hb_memory_read_u64(ctx->memory, store_addr + 8, &lane1) == HB_OK);
    ASSERT(lane0 == 0);
    ASSERT(lane1 == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_accumulator_imm_family) {
    struct {
        uint8_t code[5];
        size_t len;
        int opcode;
        uint8_t size;
    } cases[] = {
        {{0x04, 0x7f, 0, 0, 0}, 2, HB_INS_ADD, 1},
        {{0x05, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_ADD, 4},
        {{0x0c, 0x7f, 0, 0, 0}, 2, HB_INS_OR, 1},
        {{0x0d, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_OR, 4},
        {{0x14, 0x7f, 0, 0, 0}, 2, HB_INS_ADC, 1},
        {{0x15, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_ADC, 4},
        {{0x1c, 0x7f, 0, 0, 0}, 2, HB_INS_SBB, 1},
        {{0x1d, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_SBB, 4},
        {{0x24, 0x7f, 0, 0, 0}, 2, HB_INS_AND, 1},
        {{0x25, 0x00, 0x00, 0xff, 0xff}, 5, HB_INS_AND, 4},
        {{0x2c, 0x7f, 0, 0, 0}, 2, HB_INS_SUB, 1},
        {{0x2d, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_SUB, 4},
        {{0x34, 0x7f, 0, 0, 0}, 2, HB_INS_XOR, 1},
        {{0x35, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_XOR, 4},
        {{0x3c, 0x7f, 0, 0, 0}, 2, HB_INS_CMP, 1},
        {{0x3d, 0x78, 0x56, 0x34, 0x12}, 5, HB_INS_CMP, 4},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x86(cases[i].code, cases[i].len, 0x0040c000u, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.is_reg);
        ASSERT(d.op1.reg == HB_REG_RAX);
        ASSERT(d.op1.size == cases[i].size);
        ASSERT(d.op2.is_imm);
        ASSERT(d.op2.size == cases[i].size);
    }

    tests_passed++;
}

TEST(interp_x86_and_eax_imm32_accumulator) {
    const uint32_t code_base = 0x0040d000u;
    uint8_t code[] = {0x25, 0x00, 0x00, 0xff, 0xff}; /* and eax, 0xffff0000 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x12345678u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x12340000u);
    ASSERT(ctx->flags.zf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_cdq_opcode_99) {
    uint8_t code[] = {0x99}; /* cdq */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x100005961, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_CWD);
    ASSERT(d.len == 1);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 4);
    tests_passed++;
}

TEST(interp_x64_cdq_positive_and_negative) {
    uint8_t code[] = {0x99}; /* cdq */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x13;
    ctx->regs.x64.rdx = 0x12345678;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0);

    ctx->pc = base;
    ctx->regs.x64.rax = 0x80000013ULL;
    ctx->regs.x64.rdx = 0;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rdx == 0xffffffffU);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_movsx_r32_m8_calc) {
    uint8_t code[] = {0x0f, 0xbe, 0x08}; /* movsbl (%rax), %ecx */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x10000616b, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVSX);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_mem);
    ASSERT(d.op2.mem.base == HB_REG_RAX);
    ASSERT(d.op2.size == 1);
    tests_passed++;
}

TEST(interp_x64_movsx_r32_m8_negative_byte) {
    uint8_t code[] = {0x0f, 0xbe, 0x08}; /* movsbl (%rax), %ecx */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t byte = 0xf0;
    uint64_t data = (uint64_t)(uintptr_t)&byte;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data, sizeof(byte), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = data;
    ctx->regs.x64.rcx = 0xaaaaaaaaaaaaaaaaULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0xfffffff0ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_repne_scasw) {
    uint8_t code[] = {0x66, 0xf2, 0xaf}; /* repne scasw */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000060b5, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_SCAS);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0xf2);
    tests_passed++;
}

TEST(interp_x64_repne_scasw_finds_nul) {
    uint8_t code[] = {0x66, 0xf2, 0xaf}; /* repne scasw */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t wide[] = {'a', 0, 'b', 0, 0, 0};
    uint64_t data = (uint64_t)(uintptr_t)wide;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data, sizeof(wide), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = 3;
    ctx->regs.x64.rdi = data;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rdi == data + 6);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_rep_stosw_notepadpp_fill) {
    uint8_t code[] = {0x66, 0xf3, 0xab}; /* rep stosw */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x14040f828, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_STOS);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0xf3);
    tests_passed++;
}

TEST(interp_x64_rep_stosw_notepadpp_fill) {
    uint8_t code[] = {0x66, 0xf3, 0xab}; /* rep stosw */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t wide[12] = {0};
    uint64_t data = (uint64_t)(uintptr_t)wide;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data, sizeof(wide), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x0041;
    ctx->regs.x64.rcx = 3;
    ctx->regs.x64.rdi = data;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rdi == data + 6);
    ASSERT(wide[0] == 'A' && wide[1] == 0);
    ASSERT(wide[2] == 'A' && wide[3] == 0);
    ASSERT(wide[4] == 'A' && wide[5] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_rep_movsq_icon_memcpy) {
    uint8_t code[] = {0xf3, 0x48, 0xa5}; /* rep movsq */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140419600, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVS);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RSI);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0xf3);
    tests_passed++;
}

TEST(decode_rep_movsb_icon_memcpy) {
    uint8_t code[] = {0xf3, 0xa4}; /* rep movsb */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140419640, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVS);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RSI);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0xf3);
    tests_passed++;
}

TEST(interp_x64_rep_movsq_icon_memcpy_forward) {
    uint8_t code[] = {0xf3, 0x48, 0xa5}; /* rep movsq */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint64_t src_qwords[] = {
        0x1122334455667788ULL,
        0x99aabbccddeeff00ULL,
        0x0102030405060708ULL,
    };
    uint64_t dst_qwords[3] = {0};
    uint64_t src = (uint64_t)(uintptr_t)src_qwords;
    uint64_t dst = (uint64_t)(uintptr_t)dst_qwords;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, src, sizeof(src_qwords), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, dst, sizeof(dst_qwords), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 3;
    ctx->regs.x64.rsi = src;
    ctx->regs.x64.rdi = dst;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rsi == src + sizeof(src_qwords));
    ASSERT(ctx->regs.x64.rdi == dst + sizeof(dst_qwords));
    ASSERT(memcmp(dst_qwords, src_qwords, sizeof(src_qwords)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_rep_movsb_direction_flag_backward) {
    uint8_t code[] = {0xf3, 0xa4}; /* rep movsb */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t src_bytes[] = {0x10, 0x20, 0x30, 0x40};
    uint8_t dst_bytes[] = {0, 0, 0, 0};
    uint64_t src = (uint64_t)(uintptr_t)src_bytes;
    uint64_t dst = (uint64_t)(uintptr_t)dst_bytes;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, src, sizeof(src_bytes), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, dst, sizeof(dst_bytes), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = sizeof(src_bytes);
    ctx->regs.x64.rsi = src + sizeof(src_bytes) - 1;
    ctx->regs.x64.rdi = dst + sizeof(dst_bytes) - 1;
    ctx->regs.x64.rflags |= (1ULL << 10); /* Direction flag. */

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rsi == src - 1);
    ASSERT(ctx->regs.x64.rdi == dst - 1);
    ASSERT(memcmp(dst_bytes, src_bytes, sizeof(src_bytes)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_string_ops_family) {
    struct {
        const char *name;
        uint8_t code[3];
        size_t len;
        int opcode;
        uint8_t size;
        int64_t rep;
    } cases[] = {
        {"rep stosd trigger", {0xf3, 0xab, 0x00}, 2, HB_INS_STOS, 4, 0xf3},
        {"rep stosw sibling", {0x66, 0xf3, 0xab}, 3, HB_INS_STOS, 2, 0xf3},
        {"movsb sibling", {0xa4, 0x00, 0x00}, 1, HB_INS_MOVS, 1, 0},
        {"repne scasb sibling", {0xf2, 0xae, 0x00}, 2, HB_INS_SCAS, 1, 0xf2},
        {"repe cmpsd sibling", {0xf3, 0xa7, 0x00}, 2, HB_INS_CMPS, 4, 0xf3},
        {"lodsw sibling", {0x66, 0xad, 0x00}, 2, HB_INS_LODS, 2, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x86(cases[i].code, cases[i].len, 0x7bc51494u, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.size == cases[i].size);
        ASSERT(d.op2.is_imm);
        ASSERT(d.op2.imm == cases[i].rep);
        (void)cases[i].name;
    }
    tests_passed++;
}

TEST(interp_x86_rep_stosd_signal_stack_clear) {
    const uint32_t code_base = 0x00402000u;
    const uint32_t data_base = 0x00200000u;
    uint8_t code[] = {0xf3, 0xab}; /* rep stosd */
    uint32_t value = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x11223344u;
    ctx->regs.x86.ecx = 3;
    ctx->regs.x86.edi = data_base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.ecx == 0);
    ASSERT(ctx->regs.x86.edi == data_base + 12);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base, &value) == HB_OK);
    ASSERT(value == 0x11223344u);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base + 8, &value) == HB_OK);
    ASSERT(value == 0x11223344u);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_rep_movsb_direction_flag_backward) {
    const uint32_t code_base = 0x00403000u;
    const uint32_t src_base = 0x00201000u;
    const uint32_t dst_base = 0x00202000u;
    uint8_t code[] = {0xf3, 0xa4}; /* rep movsb */
    uint8_t src[] = {0x10, 0x20, 0x30, 0x40};
    uint8_t dst[4] = {0};

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, src_base, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, dst_base, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, src_base, src, sizeof(src)) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ecx = sizeof(src);
    ctx->regs.x86.esi = src_base + sizeof(src) - 1;
    ctx->regs.x86.edi = dst_base + sizeof(dst) - 1;
    ctx->regs.x86.eflags |= (1u << 10); /* Direction flag. */

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.ecx == 0);
    ASSERT(ctx->regs.x86.esi == src_base - 1);
    ASSERT(ctx->regs.x86.edi == dst_base - 1);
    ASSERT(hb_memory_read(ctx->memory, dst_base, dst, sizeof(dst)) == HB_OK);
    ASSERT(memcmp(dst, src, sizeof(src)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_repe_cmpsb_string_scan) {
    uint8_t code[] = {0xf3, 0xa6}; /* repe cmpsb */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140419680, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_CMPS);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RSI);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_imm);
    ASSERT(d.op2.imm == 0xf3);
    tests_passed++;
}

TEST(decode_lodsq_string_load) {
    uint8_t code[] = {0x48, 0xad}; /* lodsq */
    hb_decoded_t d;
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x140419690, &d);
    ASSERT(r == HB_OK);
    ASSERT(d.opcode == HB_INS_LODS);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 8);
    tests_passed++;
}

TEST(interp_x64_repe_cmpsb_stops_on_mismatch) {
    uint8_t code[] = {0xf3, 0xa6}; /* repe cmpsb */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t left[] = {0x11, 0x22, 0x33};
    uint8_t right[] = {0x11, 0x99, 0x33};
    uint64_t laddr = (uint64_t)(uintptr_t)left;
    uint64_t raddr = (uint64_t)(uintptr_t)right;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, laddr, sizeof(left), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, raddr, sizeof(right), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 3;
    ctx->regs.x64.rsi = laddr;
    ctx->regs.x64.rdi = raddr;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 1);
    ASSERT(ctx->regs.x64.rsi == laddr + 2);
    ASSERT(ctx->regs.x64.rdi == raddr + 2);
    ASSERT(ctx->flags.zf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_repne_cmpsb_stops_on_match) {
    uint8_t code[] = {0xf2, 0xa6}; /* repne cmpsb */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t left[] = {0x10, 0x20, 0x30};
    uint8_t right[] = {0x01, 0x02, 0x30};
    uint64_t laddr = (uint64_t)(uintptr_t)left;
    uint64_t raddr = (uint64_t)(uintptr_t)right;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, laddr, sizeof(left), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, raddr, sizeof(right), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 3;
    ctx->regs.x64.rsi = laddr;
    ctx->regs.x64.rdi = raddr;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rsi == laddr + 3);
    ASSERT(ctx->regs.x64.rdi == raddr + 3);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_cmpsw_direction_flag_single_step) {
    uint8_t code[] = {0x66, 0xa7}; /* cmpsw */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t left[] = {0, 0, 0x34, 0x12};
    uint8_t right[] = {0, 0, 0x34, 0x12};
    uint64_t laddr = (uint64_t)(uintptr_t)left;
    uint64_t raddr = (uint64_t)(uintptr_t)right;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, laddr, sizeof(left), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, raddr, sizeof(right), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rsi = laddr + 2;
    ctx->regs.x64.rdi = raddr + 2;
    ctx->regs.x64.rflags |= (1ULL << 10); /* Direction flag. */

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rsi == laddr);
    ASSERT(ctx->regs.x64.rdi == raddr);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_lodsd_zero_extends_eax) {
    uint8_t code[] = {0xad}; /* lodsd */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[] = {0xef, 0xcd, 0xab, 0x89};
    uint64_t addr = (uint64_t)(uintptr_t)data;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, addr, sizeof(data), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xffffffffffffffffULL;
    ctx->regs.x64.rsi = addr;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x0000000089abcdefULL);
    ASSERT(ctx->regs.x64.rsi == addr + 4);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_rep_lodsb_direction_flag_backward) {
    uint8_t code[] = {0xf3, 0xac}; /* rep lodsb */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[] = {0x10, 0x20, 0x30};
    uint64_t addr = (uint64_t)(uintptr_t)data;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, addr, sizeof(data), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xaaaaaaaaaaaaaa00ULL;
    ctx->regs.x64.rcx = sizeof(data);
    ctx->regs.x64.rsi = addr + sizeof(data) - 1;
    ctx->regs.x64.rflags |= (1ULL << 10); /* Direction flag. */

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0);
    ASSERT(ctx->regs.x64.rsi == addr - 1);
    ASSERT(ctx->regs.x64.rax == 0xaaaaaaaaaaaaaa10ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_neg_mem32_notepadpp_pointer_math) {
    uint8_t code[] = {0xf7, 0x58, 0x04}; /* negl 0x4(%rax) */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[16] = {0};
    uint64_t addr = (uint64_t)(uintptr_t)data;
    uint32_t value = 0x0000002a;
    memcpy(data + 4, &value, sizeof(value));

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, addr, sizeof(data), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = addr;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint32_t stored = 0;
    memcpy(&stored, data + 4, sizeof(stored));
    ASSERT(stored == (uint32_t)-42);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_cmp_operand16_notepadpp_mode_parser) {
    uint8_t cmp_mem_reg[] = {0x66, 0x41, 0x39, 0x28}; /* cmp %bp, (%r8) */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(cmp_mem_reg, sizeof(cmp_mem_reg), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMP);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_R8);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RBP);
    ASSERT(d.op2.size == 2);

    uint8_t cmp_reg_mem[] = {0x66, 0x41, 0x3b, 0x28}; /* cmp (%r8), %bp */
    ASSERT(hb_decode_x64(cmp_reg_mem, sizeof(cmp_reg_mem), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMP);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBP);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_R8);
    ASSERT(d.op2.size == 2);

    uint8_t cmp_ax_imm16[] = {0x66, 0x3d, 0x34, 0x12}; /* cmp $0x1234, %ax */
    ASSERT(hb_decode_x64(cmp_ax_imm16, sizeof(cmp_ax_imm16), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMP);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.size == 2);
    ASSERT((uint16_t)d.op2.imm == 0x1234U);

    tests_passed++;
}

TEST(decode_x64_alu_operand16_family) {
    hb_decoded_t d;

    uint8_t add_m16_r16[] = {0x66, 0x41, 0x01, 0x10}; /* add %dx, (%r8) */
    ASSERT(hb_decode_x64(add_m16_r16, sizeof(add_m16_r16), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADD);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_R8 && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 2);

    uint8_t sub_r16_m16[] = {0x66, 0x41, 0x2b, 0x10}; /* sub (%r8), %dx */
    ASSERT(hb_decode_x64(sub_r16_m16, sizeof(sub_r16_m16), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SUB);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_R8 && d.op2.size == 2);

    uint8_t or_ax_imm16[] = {0x66, 0x0d, 0x34, 0x12}; /* or $0x1234, %ax */
    ASSERT(hb_decode_x64(or_ax_imm16, sizeof(or_ax_imm16), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_OR);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.size == 2 && (uint16_t)d.op2.imm == 0x1234U);

    uint8_t xor_m16_r16[] = {0x66, 0x41, 0x31, 0x10}; /* xor %dx, (%r8) */
    ASSERT(hb_decode_x64(xor_m16_r16, sizeof(xor_m16_r16), 0x140408cf8ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XOR);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_R8 && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 2);

    tests_passed++;
}

TEST(interp_x64_cmp_word_mem_reg_notepadpp_je_taken) {
    uint8_t code[] = {
        0x66, 0x41, 0x39, 0x28,             /* cmp %bp, (%r8) */
        0x0f, 0x84, 0x10, 0x00, 0x00, 0x00  /* je +0x10 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[] = {0x00, 0x00, 0x77, 0x00}; /* word zero, next WCHAR 'w' */
    uint64_t data_addr = (uint64_t)(uintptr_t)data;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data_addr, sizeof(data), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.r8 = data_addr;
    ctx->regs.x64.rbp = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == base + sizeof(code) + 0x10);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_cmp_word_mem_reg_notepadpp_je_not_taken) {
    uint8_t code[] = {
        0x66, 0x41, 0x39, 0x28,             /* cmp %bp, (%r8) */
        0x0f, 0x84, 0x10, 0x00, 0x00, 0x00  /* je +0x10 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[] = {0x01, 0x00, 0x77, 0x00};
    uint64_t data_addr = (uint64_t)(uintptr_t)data;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data_addr, sizeof(data), HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.r8 = data_addr;
    ctx->regs.x64.rbp = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(out.faulted == false);
    ASSERT(ctx->pc == base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_fe_byte_inc_dec_family) {
    hb_decoded_t d;

    uint8_t dec_al[] = {0xfe, 0xc8}; /* dec %al */
    ASSERT(hb_decode_x64(dec_al, sizeof(dec_al), 0x14040d0b7ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_DEC);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    uint8_t inc_m8[] = {0x41, 0xfe, 0x00}; /* incb (%r8) */
    ASSERT(hb_decode_x64(inc_m8, sizeof(inc_m8), 0x14040d0b7ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_R8);
    ASSERT(d.op1.size == 1);

    uint8_t reserved[] = {0xfe, 0xd0}; /* FE /2 is reserved */
    ASSERT(hb_decode_x64(reserved, sizeof(reserved), 0x14040d0b7ULL, &d) == HB_ERR_UNSUPPORTED_OPCODE);

    tests_passed++;
}

TEST(decode_x86_fe_byte_inc_dec_family) {
    hb_decoded_t d;

    uint8_t dec_al[] = {0xfe, 0xc8}; /* dec al */
    ASSERT(hb_decode_x86(dec_al, sizeof(dec_al), 0x7abedb2e, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_DEC);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    uint8_t inc_m8[] = {0xfe, 0x00}; /* inc byte ptr [eax] */
    ASSERT(hb_decode_x86(inc_m8, sizeof(inc_m8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    uint8_t lock_inc_m8[] = {0xf0, 0xfe, 0x00}; /* lock inc byte ptr [eax] */
    ASSERT(hb_decode_x86(lock_inc_m8, sizeof(lock_inc_m8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    uint8_t reserved[] = {0xfe, 0xd0}; /* FE /2 is reserved */
    ASSERT(hb_decode_x86(reserved, sizeof(reserved), 0x1000, &d) == HB_ERR_UNSUPPORTED_OPCODE);

    tests_passed++;
}

TEST(interp_x86_fe_byte_inc_dec_family) {
    const uint32_t code_base = 0x00406e00u;
    const uint32_t data_base = 0x0000f000u;
    uint8_t code[] = {
        0xfe, 0x00, /* inc byte ptr [eax] */
        0xfe, 0xcb  /* dec bl */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    uint8_t value = 0xff;
    ASSERT(hb_memory_write(ctx->memory, data_base, &value, 1) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ebx = 0x12340000u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    value = 0;
    ASSERT(hb_memory_read(ctx->memory, data_base, &value, 1) == HB_OK);
    ASSERT(value == 0);
    ASSERT(ctx->regs.x86.ebx == 0x123400ffu);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_ff_inc_dec_operand_size_family) {
    hb_decoded_t d;

    uint8_t inc_r8[] = {0x49, 0xff, 0xc0}; /* inc %r8 */
    ASSERT(hb_decode_x64(inc_r8, sizeof(inc_r8), 0x14015d352ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_R8);
    ASSERT(d.op1.size == 8);

    uint8_t dec_ax[] = {0x66, 0xff, 0xc8}; /* dec %ax */
    ASSERT(hb_decode_x64(dec_ax, sizeof(dec_ax), 0x14015d352ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_DEC);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 2);

    uint8_t inc_eax[] = {0xff, 0xc0}; /* inc %eax */
    ASSERT(hb_decode_x64(inc_eax, sizeof(inc_eax), 0x14015d352ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 4);

    uint8_t call_rax[] = {0xff, 0xd0}; /* call *%rax */
    ASSERT(hb_decode_x64(call_rax, sizeof(call_rax), 0x14015d352ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CALL);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 8);

    uint8_t reserved[] = {0xff, 0xf8}; /* FF /7 is reserved */
    ASSERT(hb_decode_x64(reserved, sizeof(reserved), 0x14015d352ULL, &d) == HB_ERR_UNSUPPORTED_OPCODE);

    tests_passed++;
}

TEST(interp_x64_dec_al_notepadpp_char_class) {
    uint8_t code[] = {0xfe, 0xc8}; /* dec %al */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xdeadbeefcafeba02ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0xdeadbeefcafeba01ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_inc_m8_fe_family) {
    uint8_t code[] = {0x41, 0xfe, 0x00}; /* incb (%r8) */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint8_t data[] = {0x7f};
    uint64_t data_addr = (uint64_t)(uintptr_t)data;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, data_addr, sizeof(data), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.r8 = data_addr;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(data[0] == 0x80);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_inc_r8_rexw_notepadpp_scan_loop) {
    uint8_t code[] = {0x49, 0xff, 0xc0}; /* inc %r8 */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.r8 = 0x11223344000000ffULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(ctx->regs.x64.r8 == 0x1122334400000100ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

/* --- JIT runtime test --- */
TEST(jit_mov_add) {
    /* Build IR directly: mov rax, 5; add rax, 3 */
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);

    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(5, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(3, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 8);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_push_pop) {
    /* IR: push 0x1234; pop rax */
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_push(b, hb_ir_imm(0x1234, HB_SIZE_64));
    hb_ir_emit_pop(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;

    /* Allocate backing memory and map it into the sandbox */
    uint8_t stack_buf[4096] __attribute__((aligned(16)));
    memset(stack_buf, 0, sizeof(stack_buf));
    uintptr_t stack_top = (uintptr_t)(stack_buf + sizeof(stack_buf) - 8);
    ctx->memory = hb_memory_create(0x100000);
    hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack_buf, sizeof(stack_buf), HB_PERM_READ | HB_PERM_WRITE);
    ctx->regs.x64.rsp = stack_top;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x1234);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_stack_push_pop_family) {
    uint64_t stack[4] = {0};
    hb_ir_func_t* func = hb_ir_func_create(0x4300, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4300);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* push_reg = hb_ir_emit_push(b, hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
    hb_ir_instr_t* push_imm = hb_ir_emit_push(b, hb_ir_imm(0x1122334455667788ULL, HB_SIZE_64));
    hb_ir_instr_t* pop_rcx = hb_ir_emit_pop(b, hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
    hb_ir_instr_t* pop_rax = hb_ir_emit_pop(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64));
    ASSERT(push_reg && push_imm && pop_rcx && pop_rax);
    push_reg->guest_addr = 0x4300; push_reg->guest_len = 1;
    push_imm->guest_addr = 0x4301; push_imm->guest_len = 5;
    pop_rcx->guest_addr = 0x4306; pop_rcx->guest_len = 1;
    pop_rax->guest_addr = 0x4307; pop_rax->guest_len = 1;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack, sizeof(stack),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4300;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)&stack[2];
    ctx->regs.x64.rbx = 0xaabbccddeeff0011ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rcx == 0x1122334455667788ULL);
    ASSERT(ctx->regs.x64.rax == 0xaabbccddeeff0011ULL);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)&stack[2]);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 180);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_ret_stack_family) {
    uint64_t stack[2] = {0x445566778899aabbULL, 0};
    hb_ir_func_t* func = hb_ir_func_create(0x4400, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4400);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* ret = hb_ir_emit_ret(b);
    ASSERT(ret != NULL);
    ret->src1 = hb_ir_imm(8, HB_SIZE_16);
    ret->guest_addr = 0x4400; ret->guest_len = 3;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack, sizeof(stack),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4400;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)&stack[0];

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x445566778899aabbULL);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)&stack[2]);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(256);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 96);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_epilogue_restore_ret_block) {
    uint64_t stack[8] = {0};
    stack[4] = 0x1111222233334444ULL; /* saved rdi */
    stack[5] = 0x777788889999aaa0ULL; /* return pc */
    stack[6] = 0xaaaabbbbccccddddULL; /* saved rbx */
    stack[7] = 0x123456789abcdef0ULL; /* saved rsi */

    hb_ir_func_t* func = hb_ir_func_create(0x4500, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4500);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load_rbx = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RBX, HB_SIZE_64),
                                             hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 0x30, HB_SIZE_64));
    hb_ir_instr_t* mov_rax = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                            hb_ir_reg(HB_REG_RDI, HB_SIZE_64));
    hb_ir_instr_t* load_rsi = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                             hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 0x38, HB_SIZE_64));
    hb_ir_instr_t* add_rsp = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                              hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                              hb_ir_imm(0x20, HB_SIZE_64));
    hb_ir_instr_t* pop_rdi = hb_ir_emit_pop(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64));
    hb_ir_instr_t* ret = hb_ir_emit_ret(b);
    ASSERT(load_rbx && mov_rax && load_rsi && add_rsp && pop_rdi && ret);
    load_rbx->guest_addr = 0x4500; load_rbx->guest_len = 5;
    mov_rax->guest_addr = 0x4505; mov_rax->guest_len = 3;
    load_rsi->guest_addr = 0x4508; load_rsi->guest_len = 5;
    add_rsp->guest_addr = 0x450d; add_rsp->guest_len = 4;
    pop_rdi->guest_addr = 0x4511; pop_rdi->guest_len = 1;
    ret->guest_addr = 0x4512; ret->guest_len = 1;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack, sizeof(stack),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4500;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)&stack[0];
    ctx->regs.x64.rdi = 0x0ddc0ffeec0ffee0ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == stack[5]);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)&stack[6]);
    ASSERT(ctx->regs.x64.rbx == stack[6]);
    ASSERT(ctx->regs.x64.rsi == stack[7]);
    ASSERT(ctx->regs.x64.rdi == stack[4]);
    ASSERT(ctx->regs.x64.rax == 0x0ddc0ffeec0ffee0ULL);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 180);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_commit_verify_failure_not_marked_executable) {
    hb_jit_buffer_t* buf = hb_jit_buffer_create(4096);
    ASSERT(buf != NULL);
    ASSERT(!buf->is_executable);

    setenv("MACRUNNER_HB_TEST_FORCE_JIT_VERIFY_FAIL", "1", 1);
    ASSERT(hb_jit_buffer_commit(buf) == HB_ERR_JIT_FAILED);
    unsetenv("MACRUNNER_HB_TEST_FORCE_JIT_VERIFY_FAIL");
    ASSERT(!buf->is_executable);
    ASSERT(hb_jit_buffer_make_writable(buf) == HB_OK);
    ASSERT(!buf->is_executable);

    hb_jit_buffer_destroy(buf);
    tests_passed++;
}

TEST(jit_load_unmapped_faults) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                    hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 0, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->pc = 0x1000;
    ctx->regs.x64.rbx = 0x70000000;
    ctx->regs.x64.rax = 0x12345678;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->last_result == HB_ERR_MEMORY_FAULT);
    ASSERT(ctx->regs.x64.rax == 0x12345678);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_store_unmapped_faults) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_store(b, hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 0, HB_SIZE_64),
                     hb_ir_imm(0x55aa, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->pc = 0x1000;
    ctx->regs.x64.rbx = 0x70000000;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->last_result == HB_ERR_MEMORY_FAULT);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_control_state_store_widths) {
    uint8_t code[] = {
        0x0f, 0xae, 0x59, 0x08, /* stmxcsr 0x8(%rcx) */
        0xd9, 0x79, 0x0c        /* fnstcw  0xc(%rcx) */
    };
    uint8_t storage[16];
    memset(storage, 0xaa, sizeof(storage));
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)storage, sizeof(storage),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)storage;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(storage[7] == 0xaa);
    ASSERT(storage[8] == 0x80);
    ASSERT(storage[9] == 0x1f);
    ASSERT(storage[10] == 0x00);
    ASSERT(storage[11] == 0x00);
    ASSERT(storage[12] == 0x7f);
    ASSERT(storage[13] == 0x03);
    ASSERT(storage[14] == 0xaa);
    ASSERT(storage[15] == 0xaa);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_pop_unmapped_faults) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_pop(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->pc = 0x1000;
    ctx->regs.x64.rsp = 0x70000000;
    ctx->regs.x64.rax = 0xfeedface;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->regs.x64.rsp == 0x70000000);
    ASSERT(ctx->regs.x64.rax == 0xfeedface);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_push_guard_page_faults) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_push(b, hb_ir_imm(0x1234, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    uint8_t stack_buf[64] __attribute__((aligned(16)));
    memset(stack_buf, 0, sizeof(stack_buf));
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack_buf, sizeof(stack_buf),
                         HB_PERM_READ) == HB_OK);
    ctx->pc = 0x1000;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)(stack_buf + 8);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)(stack_buf + 8));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_call_push_fault_preserves_pc) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* call = hb_ir_emit_call(b, 0x2000);
    ASSERT(call != NULL);
    call->guest_addr = 0x1000;
    call->guest_len = 5;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->pc = 0x1000;
    ctx->regs.x64.rsp = 0x70000000;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->pc == 0x1000);
    ASSERT(ctx->regs.x64.rsp == 0x70000000);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_indirect_call_memory_reads_target) {
    uint8_t code[16] = {
        0xff, 0x15, 0x02, 0x00, 0x00, 0x00, /* callq *0x2(%rip) */
        0x90, 0x90
    };
    uint64_t target = 0x123456789abcdef0ULL;
    uint8_t stack_buf[64] __attribute__((aligned(16)));
    uint64_t ret_addr = 0;
    uint64_t base = (uint64_t)(uintptr_t)code;
    memcpy(code + 8, &target, sizeof(target));
    memset(stack_buf, 0, sizeof(stack_buf));

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack_buf, sizeof(stack_buf),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)(stack_buf + 32);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == target);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)(stack_buf + 24));
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &ret_addr) == HB_OK);
    ASSERT(ret_addr == base + 6);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_indirect_jmp_register_reads_target) {
    uint8_t code[] = {0xff, 0xe0}; /* jmpq *%rax */
    uint64_t base = (uint64_t)(uintptr_t)code;
    uint64_t target = 0x20000000ULL;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = target;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == target);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_call_push_fault_preserves_pc_and_rsp) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* call = hb_ir_emit_call(b, 0x2000);
    ASSERT(call != NULL);
    call->guest_addr = 0x1000;
    call->guest_len = 5;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->pc = 0x1000;
    ctx->regs.x64.rsp = 0x70000000;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_MEMORY_FAULT);
    ASSERT(out.faulted);
    ASSERT(ctx->pc == 0x1000);
    ASSERT(ctx->regs.x64.rsp == 0x70000000);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_jcc_helper_fault_stops_before_pc_update) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, (hb_cc_t)0xff, 0x2000);
    ASSERT(jcc != NULL);
    jcc->guest_addr = 0x1000;
    jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_ERR_UNSUPPORTED_FEATURE);
    ASSERT(out.faulted);
    ASSERT(ctx->pc == 0x1000);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(legitimate_zero_read_is_not_fault) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                    hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 0, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    uint64_t value = 0;
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&value, sizeof(value),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x1000;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&value;
    ctx->regs.x64.rax = 0xbeef;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(!out.faulted);
    ASSERT(ctx->last_result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(block_limit_env_0_unlimited) {
    char* saved = save_env_var("MACRUNNER_HB_X64_BLOCK_LIMIT");
    setenv("MACRUNNER_HB_X64_BLOCK_LIMIT", "0", 1);
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    restore_env_var("MACRUNNER_HB_X64_BLOCK_LIMIT", saved);

    ASSERT(ctx != NULL);
    ASSERT(ctx->block_limit == 0);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(block_limit_explicit_fault) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_jmp(b, 0x1000);
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->block_limit = 2;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_ERR_BLOCK_LIMIT);
    ASSERT(out.result == HB_ERR_BLOCK_LIMIT);
    ASSERT(out.faulted);
    ASSERT(out.blocks_executed == 2);
    ASSERT(ctx->last_result == HB_ERR_BLOCK_LIMIT);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(lifter_truncation_not_success) {
    const size_t code_len = 10001;
    uint8_t* code = malloc(code_len);
    ASSERT(code != NULL);
    memset(code, 0x90, code_len);
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, code_len, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->truncated);
    ASSERT(func->truncation_reason != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = base + 10000;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_ERR_TRANSLATION_TRUNCATED);
    ASSERT(out.result == HB_ERR_TRANSLATION_TRUNCATED);
    ASSERT(out.faulted);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    free(code);
    tests_passed++;
}

TEST(lifter_truncation_probe_preserves_decoder_position) {
    const size_t code_len = 10001;
    uint8_t* code = malloc(code_len);
    ASSERT(code != NULL);
    memset(code, 0x90, code_len);
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, code_len, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    ASSERT(func != NULL);
    ASSERT(func->truncated);
    ASSERT(dec->pos == 10000);
    hb_ir_func_destroy(func);
    hb_decoder_destroy(dec);

    dec = hb_decoder_create(HB_ARCH_X86, code, code_len, base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    ASSERT(func != NULL);
    ASSERT(func->truncated);
    ASSERT(dec->pos == 10000);
    hb_ir_func_destroy(func);
    hb_decoder_destroy(dec);
    free(code);
    tests_passed++;
}

TEST(notepad_plus_long_init_no_silent_abort) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_jmp(b, 0x1000);
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->block_limit = 0;
    ctx->step_limit = 3;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_ERR_STEP_LIMIT);
    ASSERT(out.result != HB_ERR_BLOCK_LIMIT);
    ASSERT(out.blocks_executed == 3);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_cmp_jcc) {
    /* IR: mov rax, 5; cmp rax, 5; je target; mov rax, 0; ret; target: mov rax, 1; ret */
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);

    hb_ir_block_t* blk1 = hb_ir_block_create(0, 0x1000);
    hb_ir_block_t* blk2 = hb_ir_block_create(1, 0x1010);
    hb_ir_cfg_add_block(func->cfg, blk1);
    hb_ir_cfg_add_block(func->cfg, blk2);
    func->cfg->entry = blk1;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk1);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(5, HB_SIZE_64));
    hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(5, HB_SIZE_64));
    hb_ir_emit_jcc(b, HB_CC_E, 0x1010);
    hb_ir_builder_set_block(b, blk2);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    /* JE should be taken because 5 == 5 */
    ASSERT(ctx->regs.x64.rax == 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_block_cache_loop) {
    /* Block A (0x1000): cmp rax, 0; je 0x1030 */
    /* Block B (0x1010): sub rax, 1; jmp 0x1000 */
    /* Block C (0x1020): ret (unreachable placeholder) */
    /* Block D (0x1030): ret (exit) */
    /* Initial rax = 3. Loop decrements to 0, then je taken. */
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);

    hb_ir_block_t* blkA = hb_ir_block_create(0, 0x1000);
    hb_ir_block_t* blkB = hb_ir_block_create(1, 0x1010);
    hb_ir_block_t* blkC = hb_ir_block_create(2, 0x1020);
    hb_ir_block_t* blkD = hb_ir_block_create(3, 0x1030);
    hb_ir_cfg_add_block(func->cfg, blkA);
    hb_ir_cfg_add_block(func->cfg, blkB);
    hb_ir_cfg_add_block(func->cfg, blkC);
    hb_ir_cfg_add_block(func->cfg, blkD);
    func->cfg->entry = blkA;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blkA);
    hb_ir_instr_t* cmpA = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(0, HB_SIZE_64));
    cmpA->guest_addr = 0x1000; cmpA->guest_len = 10;
    hb_ir_instr_t* jccA = hb_ir_emit_jcc(b, HB_CC_E, 0x1030);
    jccA->guest_addr = 0x100A; jccA->guest_len = 6;
    hb_ir_builder_set_block(b, blkB);
    hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_emit_jmp(b, 0x1000);
    hb_ir_builder_set_block(b, blkC);
    hb_ir_builder_set_block(b, blkD);
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->regs.x64.rax = 3;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0);
    /* Block A executed 4 times, Block B 3 times, Block D once */
    ASSERT(out.blocks_executed == 8);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_jcc_not_taken) {
    /* Block A (0x1000): mov rax, 3; cmp rax, 3; jne 0x1020 */
    /* Block B (0x1010): mov rax, 99; ret */
    /* Block C (0x1020): mov rax, 1; ret */
    /* jne should NOT be taken because 3 == 3 */
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);

    hb_ir_block_t* blkA = hb_ir_block_create(0, 0x1000);
    hb_ir_block_t* blkB = hb_ir_block_create(1, 0x1010);
    hb_ir_block_t* blkC = hb_ir_block_create(2, 0x1020);
    hb_ir_cfg_add_block(func->cfg, blkA);
    hb_ir_cfg_add_block(func->cfg, blkB);
    hb_ir_cfg_add_block(func->cfg, blkC);
    func->cfg->entry = blkA;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blkA);
    hb_ir_instr_t* movA = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(3, HB_SIZE_64));
    movA->guest_addr = 0x1000; movA->guest_len = 10;
    hb_ir_instr_t* cmpA = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(3, HB_SIZE_64));
    cmpA->guest_addr = 0x100A; cmpA->guest_len = 4;
    hb_ir_instr_t* jccA = hb_ir_emit_jcc(b, HB_CC_NE, 0x1020);
    jccA->guest_addr = 0x100E; jccA->guest_len = 2;
    hb_ir_builder_set_block(b, blkB);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(99, HB_SIZE_64));
    hb_ir_builder_set_block(b, blkC);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;

    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    /* jne NOT taken -> fallthrough to block B -> rax = 99 */
    ASSERT(ctx->regs.x64.rax == 99);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(aot_cache_roundtrip) {
    (void)system("mkdir -p build/hyperbridge-cache");
    const char* path = "build/hyperbridge-cache/hb_aot_test.cache";
    (void)remove(path);

    hb_cache_t* cache = hb_cache_create(path);
    ASSERT(cache != NULL);

    /* Compute a key */
    uint8_t code[] = {0x48, 0x89, 0xc8}; /* mov rax, rcx */
    hb_cache_key_t key;
    hb_result_t r = hb_cache_key_compute(code, sizeof(code), HB_ARCH_X64, 1, &key);
    ASSERT(r == HB_OK);
    ASSERT(key.code_hash != 0);

    /* Create an entry */
    hb_cache_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.key = key;
    entry.valid = true;
    entry.unsupported = false;
    entry.steps = 7;
    uint8_t native[] = {0xd5, 0x03, 0x20, 0x1f, 0xd6, 0x5f, 0x03, 0xc0};
    entry.native_code = native;
    entry.native_size = sizeof(native);

    /* Put */
    r = hb_cache_put(cache, &key, &entry);
    ASSERT(r == HB_OK);

    /* Get */
    hb_cache_entry_t* got = NULL;
    r = hb_cache_get(cache, &key, &got);
    ASSERT(r == HB_OK);
    ASSERT(got != NULL);
    ASSERT(got->valid == true);
    ASSERT(got->steps == 7);
    ASSERT(got->native_size == sizeof(native));
    ASSERT(memcmp(got->native_code, native, sizeof(native)) == 0);

    /* Invalidate with different version */
    r = hb_cache_invalidate(cache, 2);
    ASSERT(r == HB_OK);

    /* Should not find entry with version 1 */
    hb_cache_entry_t* got2 = NULL;
    r = hb_cache_get(cache, &key, &got2);
    ASSERT(r == HB_ERR_NOT_FOUND);

    /* Clear */
    r = hb_cache_clear(cache);
    ASSERT(r == HB_OK);

    hb_cache_entry_free(got);
    hb_cache_destroy(cache);
    remove(path);
    tests_passed++;
}

TEST(aot_cache_sync_failure_unlinks_temp) {
    (void)system("mkdir -p build/hyperbridge-cache");
    const char* path = "build/hyperbridge-cache/hb_aot_sync_fail.cache";
    (void)remove(path);

    hb_cache_t* cache = hb_cache_create(path);
    ASSERT(cache != NULL);

    uint8_t code[] = {0x48, 0x89, 0xc8};
    hb_cache_key_t key;
    ASSERT(hb_cache_key_compute(code, sizeof(code), HB_ARCH_X64, 11, &key) == HB_OK);

    hb_cache_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.key = key;
    entry.valid = true;
    entry.steps = 1;
    uint8_t native[] = {0xd6, 0x5f, 0x03, 0xc0};
    entry.native_code = native;
    entry.native_size = sizeof(native);

    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    setenv("MACRUNNER_HB_TEST_FORCE_AOT_SYNC_FAIL", "1", 1);
    ASSERT(hb_cache_put(cache, &key, &entry) == HB_ERR_NOT_FOUND);
    unsetenv("MACRUNNER_HB_TEST_FORCE_AOT_SYNC_FAIL");
    ASSERT(access(tmp, F_OK) != 0);

    hb_cache_destroy(cache);
    remove(path);
    tests_passed++;
}

TEST(abi_stack_setup) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    hb_result_t r = hb_abi_setup_stack(ctx, 65536);
    ASSERT(r == HB_OK);
    ASSERT(ctx->regs.x64.rsp == ctx->memory->stack_top);
    ASSERT(ctx->memory->stack_top != 0);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(abi_x64_call_setup) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    hb_abi_setup_stack(ctx, 65536);

    hb_abi_x64_call_t call = {0};
    call.rcx = 1;
    call.rdx = 2;
    call.r8 = 3;
    call.r9 = 4;
    call.stack_args = NULL;
    call.stack_arg_count = 0;

    hb_result_t r = hb_abi_x64_call(ctx, 0x401000, &call, NULL);
    ASSERT(r == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 1);
    ASSERT(ctx->regs.x64.rdx == 2);
    ASSERT(ctx->regs.x64.r8 == 3);
    ASSERT(ctx->regs.x64.r9 == 4);
    ASSERT(ctx->pc == 0x401000);

    /* Verify return address sentinel was pushed */
    uint64_t ret_addr = 0;
    r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &ret_addr);
    ASSERT(r == HB_OK);
    ASSERT(ret_addr == 0xFFFF0000);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(abi_x64_call_stack_args_shadow_space) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    hb_abi_setup_stack(ctx, 65536);

    uint64_t stack_args[2] = {5, 6};
    hb_abi_x64_call_t call = {0};
    call.rcx = 1;
    call.rdx = 2;
    call.r8 = 3;
    call.r9 = 4;
    call.stack_args = stack_args;
    call.stack_arg_count = 2;
    call.shadow_space[0] = 0x1111;
    call.shadow_space[1] = 0x2222;

    ASSERT(hb_abi_x64_call(ctx, 0x401000, &call, NULL) == HB_OK);
    ASSERT((ctx->regs.x64.rsp & 0xf) == 8);

    uint64_t value = 0;
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &value) == HB_OK);
    ASSERT(value == 0xFFFF0000);
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 8, &value) == HB_OK);
    ASSERT(value == 0x1111);
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 16, &value) == HB_OK);
    ASSERT(value == 0x2222);
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 0x28, &value) == HB_OK);
    ASSERT(value == 5);
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 0x30, &value) == HB_OK);
    ASSERT(value == 6);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(abi_x64_call_leaves_positive_stack_headroom) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(hb_abi_setup_stack(ctx, 65536) == HB_OK);

    hb_abi_x64_call_t call = {0};
    ASSERT(hb_abi_x64_call(ctx, 0x401000, &call, NULL) == HB_OK);

    ASSERT(ctx->regs.x64.rsp + 0x180 < ctx->memory->stack_top);
    ASSERT(hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp + 0x78, 0x12345678) == HB_OK);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(x64_abi_setup_stack_unmapped_returns_fault) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ctx->regs.x64.rsp = 0x70000000;
    ctx->regs.x64.rcx = 0xaaaa;
    ctx->pc = 0x1111;

    hb_abi_x64_call_t call = {0};
    call.rcx = 1;
    hb_result_t r = hb_abi_x64_call(ctx, 0x401000, &call, NULL);
    ASSERT(r == HB_ERR_MEMORY_FAULT);
    ASSERT(ctx->regs.x64.rsp == 0x70000000);
    ASSERT(ctx->regs.x64.rcx == 0xaaaa);
    ASSERT(ctx->pc == 0x1111);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(x64_abi_setup_shadow_space_guard_page_returns_fault) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(hb_abi_setup_stack(ctx, 65536) == HB_OK);
    ASSERT(hb_memory_protect(ctx->memory, ctx->memory->stack_bottom, 65536, HB_PERM_READ) == HB_OK);
    uint64_t old_rsp = ctx->regs.x64.rsp;

    hb_abi_x64_call_t call = {0};
    call.shadow_space[0] = 0x1111;
    hb_result_t r = hb_abi_x64_call(ctx, 0x401000, &call, NULL);
    ASSERT(r == HB_ERR_MEMORY_FAULT);
    ASSERT(ctx->regs.x64.rsp == old_rsp);
    ASSERT(ctx->pc != 0x401000);

    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(x64_abi_setup_success_writes_return_shadow_stack_args) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(hb_abi_setup_stack(ctx, 65536) == HB_OK);

    uint64_t stack_args[3] = {0x5555, 0x6666, 0x7777};
    hb_abi_x64_call_t call = {0};
    call.rcx = 1;
    call.rdx = 2;
    call.r8 = 3;
    call.r9 = 4;
    call.shadow_space[0] = 0x1111;
    call.shadow_space[1] = 0x2222;
    call.shadow_space[2] = 0x3333;
    call.shadow_space[3] = 0x4444;
    call.stack_args = stack_args;
    call.stack_arg_count = 3;

    ASSERT(hb_abi_x64_call(ctx, 0x401000, &call, NULL) == HB_OK);
    ASSERT((ctx->regs.x64.rsp & 0xf) == 8);
    ASSERT(ctx->regs.x64.rcx == 1);
    ASSERT(ctx->regs.x64.rdx == 2);
    ASSERT(ctx->regs.x64.r8 == 3);
    ASSERT(ctx->regs.x64.r9 == 4);
    ASSERT(ctx->pc == 0x401000);
    ASSERT(ctx->regs.x64.rip == 0x401000);

    uint64_t value = 0;
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &value) == HB_OK);
    ASSERT(value == 0xFFFF0000);
    for (size_t i = 0; i < 4; i++) {
        ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 8 + i * 8, &value) == HB_OK);
        ASSERT(value == call.shadow_space[i]);
    }
    for (size_t i = 0; i < call.stack_arg_count; i++) {
        ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp + 0x28 + i * 8, &value) == HB_OK);
        ASSERT(value == stack_args[i]);
    }

    hb_context_destroy(ctx);
    tests_passed++;
}

static uint64_t test_host_add_u64(uint64_t a, uint64_t b) {
    return a + b;
}

static uint32_t test_host_inc_u32(uint32_t a) {
    return a + 1;
}

static uint64_t test_host_get_std_handle(void) {
    return 0x12345678ULL;
}

static uint32_t test_host_write_file(uint64_t handle, uint64_t buffer, uint32_t len, uint64_t written, uint64_t overlapped) {
    (void)handle;
    (void)buffer;
    (void)overlapped;
    if (written) *(uint32_t*)(uintptr_t)written = len;
    return 1;
}

static uint32_t last_exit_code;
static void test_host_exit_process(uint32_t code) {
    last_exit_code = code;
}

static int32_t test_host_wsprintf_like(uintptr_t dst, uintptr_t fmt, uint64_t a, uint64_t b) {
    return snprintf((char*)dst, 128, (const char*)fmt, (unsigned long long)a, (unsigned long long)b);
}

static void test_original_import(void) { }
static void test_bridge_import(void) { }

TEST(translation_cache_api_stats_and_module_invalidate) {
    (void)system("rm -rf build/hyperbridge-cache && mkdir -p build/hyperbridge-cache");
    hb_cache_options_t opts = {0};
    hb_cache_t* cache = hb_cache_open("build/hyperbridge-cache", &opts);
    ASSERT(cache != NULL);

    uint8_t code[] = {0x48, 0x89, 0xc8, 0xc3};
    hb_cache_key_t key;
    hb_result_t r = hb_cache_key_compute(code, sizeof(code), HB_ARCH_X64, 7, &key);
    ASSERT(r == HB_OK);
    key.module_id = 0xabc;
    key.guest_addr = 0x401000;

    uint8_t native[] = {0x1f, 0x20, 0x03, 0xd5};
    hb_cache_entry_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.steps = 3;
    r = hb_cache_store(cache, &key, native, sizeof(native), &meta);
    ASSERT(r == HB_OK);

    hb_cache_entry_t* got = NULL;
    r = hb_cache_lookup(cache, &key, &got);
    ASSERT(r == HB_OK);
    ASSERT(got != NULL);
    ASSERT(got->native_size == sizeof(native));
    ASSERT(memcmp(got->native_code, native, sizeof(native)) == 0);
    hb_cache_entry_free(got);

    hb_cache_stats_t stats;
    r = hb_cache_stats(cache, &stats);
    ASSERT(r == HB_OK);
    ASSERT(stats.hits >= 1);
    ASSERT(stats.bytes_stored >= sizeof(native));

    r = hb_cache_invalidate_module(cache, 0xabc);
    ASSERT(r == HB_OK);
    got = NULL;
    r = hb_cache_lookup(cache, &key, &got);
    ASSERT(r == HB_ERR_NOT_FOUND);
    hb_cache_close(cache);
    tests_passed++;
}

TEST(marker_filters_and_jsonl) {
    (void)system("mkdir -p build");
    const char* path = "build/hb_marker_test.jsonl";
    remove(path);
    ASSERT(hb_marker_section_allowed(HB_PE_MACHINE_AMD64, HB_PE_SECTION_EXECUTE, 0x1000, false));
    ASSERT(!hb_marker_section_allowed(HB_PE_MACHINE_ARM64, HB_PE_SECTION_EXECUTE, 0x1000, false));
    ASSERT(!hb_marker_section_allowed(HB_PE_MACHINE_I386, HB_PE_SECTION_EXECUTE | HB_PE_SECTION_WRITE, 0x1000, false));

    hb_wine_pe_section_marker_t marker;
    memset(&marker, 0, sizeof(marker));
    marker.abi_version = HB_WINE_PE_MARKER_ABI_VERSION;
    marker.machine = HB_PE_MACHINE_AMD64;
    marker.module_id = 42;
    marker.image_base = 0x140000000ULL;
    marker.section_rva = 0x1000;
    marker.section_size = 0x2000;
    marker.characteristics = HB_PE_SECTION_EXECUTE;
    strcpy(marker.module_path, "fixture.dll");
    ASSERT(hb_marker_emit_jsonl(path, HB_MARKER_EVENT_LOAD, &marker, "test") == HB_OK);
    ASSERT(hb_marker_emit_unload_jsonl(path, 42, marker.image_base, "fixture.dll", "unit-test") == HB_OK);
    FILE* fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    fseek(fp, 0, SEEK_END);
    ASSERT(ftell(fp) > 40);
    fclose(fp);
    remove(path);
    tests_passed++;
}

TEST(iat_rewriter_apply_and_rollback) {
    hb_iat_rewrite_plan_t plan;
    hb_iat_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT(hb_iat_plan_init(&plan, 4) == HB_OK);
    void* slot = (void*)test_original_import;
    hb_iat_rewrite_entry_t e;
    memset(&e, 0, sizeof(e));
    e.module_id = 7;
    e.iat_rva = 0x1234;
    e.slot = &slot;
    e.bridge_target = (void*)test_bridge_import;
    e.guest_machine = HB_PE_MACHINE_AMD64;
    e.flags = HB_IAT_FLAG_ALLOWLISTED;
    strcpy(e.dll_name, "kernel32.dll");
    strcpy(e.import_name, "GetTickCount");
    ASSERT(hb_iat_plan_add(&plan, &e) == HB_OK);
    ASSERT(hb_iat_plan_apply(&plan, true, &stats) == HB_OK);
    ASSERT(slot == (void*)test_original_import);
    ASSERT(stats.rewritten_count == 1);
    ASSERT(hb_iat_plan_apply(&plan, false, &stats) == HB_OK);
    ASSERT(slot == (void*)test_bridge_import);
    ASSERT(hb_iat_plan_rollback(&plan, &stats) == HB_OK);
    ASSERT(slot == (void*)test_original_import);
    ASSERT(stats.rollback_count == 1);
    hb_iat_plan_destroy(&plan);
    tests_passed++;
}

TEST(iat_rewriter_denied_and_bridge_stub) {
    hb_iat_rewrite_plan_t plan;
    hb_iat_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT(hb_iat_plan_init(&plan, 2) == HB_OK);
    void* slot = (void*)test_original_import;
    hb_iat_rewrite_entry_t e;
    memset(&e, 0, sizeof(e));
    e.module_id = 8;
    e.iat_rva = 0x5678;
    e.slot = &slot;
    e.bridge_target = NULL; /* Missing bridge must leave Wine behavior unchanged. */
    e.guest_machine = HB_PE_MACHINE_AMD64;
    strcpy(e.dll_name, "kernel32.dll");
    strcpy(e.import_name, "DeniedImport");
    ASSERT(hb_iat_plan_add(&plan, &e) == HB_OK);
    ASSERT(hb_iat_plan_apply(&plan, false, &stats) == HB_OK);
    ASSERT(slot == (void*)test_original_import);
    ASSERT(stats.denied_count == 1);
    uint64_t a = hb_call_import(8, 1, NULL);
    uint64_t b = hb_call_import(8, 1, NULL);
    ASSERT(b == a + 1);
    hb_iat_plan_destroy(&plan);
    tests_passed++;
}

TEST(imports_bound_import_walk_by_section_size) {
    uint8_t image[256];
    memset(image, 0, sizeof(image));
    hb_pe_image_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.mapped_image = image;
    pe.mapped_size = sizeof(image);
    pe.data_directory[HB_PE_DD_IMPORT][0] = 0x20;
    pe.data_directory[HB_PE_DD_IMPORT][1] = 32;

    hb_pe_import_desc_t* desc = (hb_pe_import_desc_t*)(void*)(image + 0x20);
    desc->original_first_thunk = 0x80;
    desc->first_thunk = 0xa0;
    desc->name_rva = 0xc0;
    strcpy((char*)image + 0xc0, "kernel32.dll");

    uint64_t* orig = (uint64_t*)(void*)(image + 0x80);
    for (size_t i = 0; i < 4; i++) orig[i] = 0x8000000000000001ULL;

    hb_thunk_table_t* table = hb_thunk_table_create();
    ASSERT(table != NULL);
    ASSERT(hb_imports_resolve(&pe, table) == HB_ERR_PE_PARSE);
    hb_thunk_table_destroy(table);
    tests_passed++;
}

TEST(imports_resolve_writes_guest_visible_thunk_target) {
    uint8_t image[512];
    memset(image, 0, sizeof(image));

    hb_pe_image_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.mapped_image = image;
    pe.mapped_size = sizeof(image);
    pe.data_directory[HB_PE_DD_IMPORT][0] = 0x20;
    pe.data_directory[HB_PE_DD_IMPORT][1] = sizeof(hb_pe_import_desc_t) * 2;

    hb_pe_import_desc_t* desc = (hb_pe_import_desc_t*)(image + 0x20);
    desc[0].original_first_thunk = 0x80;
    desc[0].first_thunk = 0xa0;
    desc[0].name_rva = 0xc0;
    strcpy((char*)(image + 0xc0), "kernel32.dll");

    uint64_t* orig_thunk = (uint64_t*)(image + 0x80);
    uint64_t* iat = (uint64_t*)(image + 0xa0);
    orig_thunk[0] = 0xd0;
    orig_thunk[1] = 0;
    image[0xd0] = 0;
    image[0xd1] = 0;
    strcpy((char*)(image + 0xd2), "GetTickCount");

    hb_thunk_table_t* table = hb_thunk_table_create();
    ASSERT(table != NULL);
    hb_thunk_def_t def;
    memset(&def, 0, sizeof(def));
    def.id = 7;
    def.dll_name = "kernel32.dll";
    def.func_name = "GetTickCount";
    def.fn = NULL;
    def.description = "guest target regression";

    ASSERT(hb_thunk_register(table, &def) == HB_OK);
    ASSERT(hb_imports_resolve(&pe, table) == HB_OK);
    ASSERT(iat[0] == hb_thunk_guest_target_from_id(7));
    ASSERT(iat[0] != 7);

    hb_thunk_table_destroy(table);
    tests_passed++;
}

TEST(iat_rewrite_readonly_page_fails_cleanly) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    ASSERT(page >= sizeof(void*));
    void** slot = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(slot != MAP_FAILED);
    *slot = (void*)test_original_import;
    ASSERT(mprotect(slot, page, PROT_READ) == 0);

    hb_iat_rewrite_plan_t plan;
    hb_iat_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT(hb_iat_plan_init(&plan, 1) == HB_OK);
    hb_iat_rewrite_entry_t e;
    memset(&e, 0, sizeof(e));
    e.module_id = 16;
    e.iat_rva = 0x1000;
    e.slot = slot;
    e.bridge_target = (void*)test_bridge_import;
    e.guest_machine = HB_PE_MACHINE_AMD64;
    e.flags = HB_IAT_FLAG_ALLOWLISTED;
    strcpy(e.dll_name, "kernel32.dll");
    strcpy(e.import_name, "ReadOnlyImport");
    ASSERT(hb_iat_plan_add(&plan, &e) == HB_OK);

    ASSERT(hb_iat_plan_apply(&plan, false, &stats) == HB_ERR_MEMORY_FAULT);
    ASSERT(*slot == (void*)test_original_import);
    ASSERT(stats.rewritten_count == 0);
    ASSERT((plan.entries[0].flags & HB_IAT_FLAG_APPLIED) == 0);

    ASSERT(mprotect(slot, page, PROT_READ | PROT_WRITE) == 0);
    hb_iat_plan_destroy(&plan);
    munmap(slot, page);
    tests_passed++;
}

TEST(iat_rewrite_writable_page_passes) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    ASSERT(page >= sizeof(void*));
    void** slot = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(slot != MAP_FAILED);
    *slot = (void*)test_original_import;

    hb_iat_rewrite_plan_t plan;
    hb_iat_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT(hb_iat_plan_init(&plan, 1) == HB_OK);
    hb_iat_rewrite_entry_t e;
    memset(&e, 0, sizeof(e));
    e.module_id = 17;
    e.iat_rva = 0x1008;
    e.slot = slot;
    e.bridge_target = (void*)test_bridge_import;
    e.guest_machine = HB_PE_MACHINE_AMD64;
    e.flags = HB_IAT_FLAG_ALLOWLISTED;
    strcpy(e.dll_name, "kernel32.dll");
    strcpy(e.import_name, "WritableImport");
    ASSERT(hb_iat_plan_add(&plan, &e) == HB_OK);

    ASSERT(hb_iat_plan_apply(&plan, false, &stats) == HB_OK);
    ASSERT(*slot == (void*)test_bridge_import);
    ASSERT(stats.rewritten_count == 1);
    ASSERT((plan.entries[0].flags & HB_IAT_FLAG_APPLIED) != 0);
    ASSERT(hb_iat_plan_rollback(&plan, &stats) == HB_OK);
    ASSERT(*slot == (void*)test_original_import);

    hb_iat_plan_destroy(&plan);
    munmap(slot, page);
    tests_passed++;
}

TEST(abi_thunk_generator_x64_and_registry) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->regs.x64.rcx = 20;
    ctx->regs.x64.rdx = 22;
    hb_generated_thunk_t thunk;
    ASSERT(hb_thunk_get(ctx, 9, HB_THUNK_SIG_U64_U64_U64, (void*)test_host_add_u64, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(ctx->regs.x64.rax == 42);
    hb_generated_thunk_t thunk2;
    ASSERT(hb_thunk_get(ctx, 9, HB_THUNK_SIG_U64_U64_U64, (void*)test_host_add_u64, &thunk2) == HB_OK);
    ASSERT(thunk2.generation == thunk.generation);
    hb_thunk_stats_t stats;
    ASSERT(hb_thunk_stats(&stats) == HB_OK);
    ASSERT(stats.generated >= 1);
    ASSERT(stats.cache_hits >= 1);
    ASSERT(!stats.wx_pages_used);
    ASSERT(hb_thunk_release(9) == HB_OK);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(abi_thunk_generator_x86_signature) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    hb_generated_thunk_t thunk;
    ASSERT(hb_thunk_get(ctx, 10, HB_THUNK_SIG_U32_U32, (void*)test_host_inc_u32, &thunk) == HB_OK);
    ASSERT(thunk.valid);
    ASSERT(thunk.signature_id == HB_THUNK_SIG_U32_U32);
    ASSERT(hb_thunk_release(10) == HB_OK);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(abi_thunk_generator_win32_shapes) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x20000);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_setup_heap(ctx->memory, 0, 8192) == HB_OK);
    ASSERT(hb_memory_setup_stack(ctx->memory, 0, 8192) == HB_OK);
    ctx->regs.x64.rsp = ctx->memory->stack_top - 128;

    hb_generated_thunk_t thunk;
    ASSERT(hb_thunk_get(ctx, 11, HB_THUNK_SIG_U64_VOID, (void*)test_host_get_std_handle, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x12345678ULL);

    uint64_t written_ptr = ctx->memory->heap_base + 64;
    ctx->regs.x64.rcx = 0x12345678ULL;
    ctx->regs.x64.rdx = ctx->memory->heap_base + 128;
    ctx->regs.x64.r8 = 37;
    ctx->regs.x64.r9 = written_ptr;
    ASSERT(hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp + 40, 0) == HB_OK);
    ASSERT(hb_thunk_get(ctx, 11, HB_THUNK_SIG_BOOL_HANDLE_PTR_U32_PTR_PTR, (void*)test_host_write_file, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(ctx->regs.x64.rax == 1);
    uint32_t written = 0;
    ASSERT(hb_memory_read_u32(ctx->memory, written_ptr, &written) == HB_OK);
    ASSERT(written == 37);

    last_exit_code = 0;
    ctx->regs.x64.rcx = 42;
    ASSERT(hb_thunk_get(ctx, 11, HB_THUNK_SIG_VOID_U32, (void*)test_host_exit_process, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(last_exit_code == 42);

    char* dst = (char*)(uintptr_t)(ctx->memory->heap_base + 256);
    char* fmt = (char*)(uintptr_t)(ctx->memory->heap_base + 512);
    strcpy(fmt, "var %llu %llu");
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.rdx = (uint64_t)(uintptr_t)fmt;
    ctx->regs.x64.r8 = 7;
    ctx->regs.x64.r9 = 9;
    ASSERT(hb_thunk_get(ctx, 11, HB_THUNK_SIG_I32_PTR_CSTR_U64_U64, (void*)test_host_wsprintf_like, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(ctx->regs.x64.rax == strlen("var 7 9"));
    ASSERT(strcmp(dst, "var 7 9") == 0);

    hb_thunk_stats_t stats;
    ASSERT(hb_thunk_stats(&stats) == HB_OK);
    ASSERT(!stats.wx_pages_used);
    ASSERT(hb_thunk_release(11) == HB_OK);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(thunk_rejects_guest_va) {
    hb_generated_thunk_t thunk;
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_thunk_get(ctx, 12, HB_THUNK_SIG_VOID_VOID, (void*)0x401000, &thunk) == HB_ERR_INVALID_ARG);
    void* fn = (void*)test_host_get_std_handle;
    ASSERT(hb_thunk_get(ctx, 12, HB_THUNK_SIG_U64_VOID, fn, &thunk) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)fn, 64,
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_thunk_get(ctx, 12, HB_THUNK_SIG_U64_VOID, fn, &thunk) == HB_ERR_INVALID_ARG);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_ERR_INVALID_ARG);
    ASSERT(hb_thunk_release(12) == HB_OK);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(thunk_rejects_null) {
    hb_generated_thunk_t thunk;
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ASSERT(hb_thunk_get(NULL, 13, HB_THUNK_SIG_U32_U32, (void*)test_host_inc_u32, &thunk) == HB_ERR_INVALID_ARG);
    ASSERT(hb_thunk_get(ctx, 13, HB_THUNK_SIG_VOID_VOID, NULL, &thunk) == HB_ERR_INVALID_ARG);
    memset(&thunk, 0, sizeof(thunk));
    thunk.module_id = 13;
    thunk.signature_id = HB_THUNK_SIG_VOID_VOID;
    thunk.valid = true;
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_ERR_INVALID_ARG);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(thunk_rejects_data_pointer) {
    static uint64_t data_target;
    hb_generated_thunk_t thunk;
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ASSERT(hb_thunk_get(ctx, 14, HB_THUNK_SIG_U64_VOID, (void*)&data_target, &thunk) == HB_ERR_INVALID_ARG);
    memset(&thunk, 0, sizeof(thunk));
    thunk.module_id = 14;
    thunk.signature_id = HB_THUNK_SIG_U64_VOID;
    thunk.target_ptr = &data_target;
    thunk.native_entry = &data_target;
    thunk.valid = true;
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_ERR_INVALID_ARG);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(thunk_accepts_registered_host_fn) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->regs.x64.rcx = 41;

    hb_generated_thunk_t thunk;
    ASSERT(hb_thunk_get(ctx, 15, HB_THUNK_SIG_U32_U32, (void*)test_host_inc_u32, &thunk) == HB_OK);
    ASSERT(hb_thunk_call_generated(ctx, &thunk) == HB_OK);
    ASSERT(ctx->regs.x64.rax == 42);
    ASSERT(hb_thunk_release(15) == HB_OK);
    hb_context_destroy(ctx);
    tests_passed++;
}

TEST(page_fault_dispatcher_ownership_dirty_unload) {
    hb_fault_dispatcher_t d;
    hb_fault_result_t out;
    ASSERT(hb_fault_dispatcher_init(&d, 8) == HB_OK);
    ASSERT(hb_fault_lazy_translate(&d, 3, 0x401000, 0x70000000, 0x70000020) == HB_OK);
    ASSERT(hb_fault_classify(&d, 0x401008, true, false, &out) == HB_OK);
    ASSERT(out.fault_class == HB_FAULT_CLASS_EXECUTE_GUEST);
    ASSERT(hb_fault_classify(&d, 0x70000004, true, false, &out) == HB_OK);
    ASSERT(out.fault_class == HB_FAULT_CLASS_NATIVE_BLOCK);
    ASSERT(out.guest_pc == 0x401004);
    ASSERT(hb_fault_mark_dirty(&d, 3, 0x401000) == HB_OK);
    ASSERT(hb_fault_classify(&d, 0x70000004, true, false, &out) == HB_OK);
    ASSERT(out.fault_class == HB_FAULT_CLASS_STALE_BLOCK);
    ASSERT(hb_fault_unload_module(&d, 3) == HB_OK);
    ASSERT(hb_fault_classify(&d, 0x70000004, true, false, &out) == HB_OK);
    ASSERT(out.fault_class == HB_FAULT_CLASS_OUTSIDE);
    hb_fault_dispatcher_destroy(&d);
    tests_passed++;
}

TEST(page_fault_dispatcher_stress_and_outside) {
    hb_fault_dispatcher_t d;
    hb_fault_result_t out;
    ASSERT(hb_fault_dispatcher_init(&d, 64) == HB_OK);
    for (uint64_t i = 0; i < 16; i++) {
        ASSERT(hb_fault_lazy_translate(&d, 100 + i, 0x500000 + i * 0x1000,
                                       0x80000000 + i * 0x100, 0x80000040 + i * 0x100) == HB_OK);
    }
    ASSERT(hb_fault_classify(&d, 0xDEADBEEF, true, false, &out) == HB_OK);
    ASSERT(out.fault_class == HB_FAULT_CLASS_OUTSIDE);
    for (uint64_t i = 0; i < 100000; i++) {
        uint64_t idx = i & 15;
        uint64_t guest_pc = 0x500000 + idx * 0x1000 + (i & 0x3f);
        uint64_t native_pc = 0x80000000 + idx * 0x100 + (i & 0x3f);
        ASSERT(hb_fault_classify(&d, guest_pc, true, false, &out) == HB_OK);
        ASSERT(out.fault_class == HB_FAULT_CLASS_EXECUTE_GUEST);
        ASSERT(hb_fault_classify(&d, native_pc, true, false, &out) == HB_OK);
        ASSERT(out.fault_class == HB_FAULT_CLASS_NATIVE_BLOCK);
    }
    ASSERT(d.lazy_translations == 16);
    ASSERT(d.stale_rejections == 0);
    hb_fault_dispatcher_destroy(&d);
    tests_passed++;
}

static uint8_t* build_minimal_pe64(size_t* out_size) {
    size_t hdr_size = 0x200;
    size_t sec_size = 0x200;
    size_t total = hdr_size + sec_size;
    uint8_t* buf = calloc(1, total);

    /* DOS header */
    *(uint16_t*)(buf + 0x00) = 0x5A4D;
    *(uint32_t*)(buf + 0x3C) = 0x40;

    /* PE signature */
    buf[0x40] = 'P'; buf[0x41] = 'E'; buf[0x42] = 0; buf[0x43] = 0;

    /* COFF header */
    *(uint16_t*)(buf + 0x44) = 0x8664; /* machine AMD64 */
    *(uint16_t*)(buf + 0x46) = 1;       /* number_of_sections */
    *(uint32_t*)(buf + 0x48) = 0;       /* time_date_stamp */
    *(uint32_t*)(buf + 0x4C) = 0;       /* pointer_to_symbol_table */
    *(uint32_t*)(buf + 0x50) = 0;       /* number_of_symbols */
    *(uint16_t*)(buf + 0x54) = 240;     /* size_of_optional_header */
    *(uint16_t*)(buf + 0x56) = 0x2022;  /* characteristics */

    /* Optional header (PE32+) */
    *(uint16_t*)(buf + 0x58) = 0x20b;   /* magic */
    buf[0x5A] = 1; buf[0x5B] = 0;       /* linker version */
    *(uint32_t*)(buf + 0x5C) = 0x200;   /* size_of_code */
    *(uint32_t*)(buf + 0x60) = 0;       /* size_of_initialized_data */
    *(uint32_t*)(buf + 0x64) = 0;       /* size_of_uninitialized_data */
    *(uint32_t*)(buf + 0x68) = 0x1000;  /* entry_point */
    *(uint32_t*)(buf + 0x6C) = 0x1000;  /* base_of_code */
    *(uint64_t*)(buf + 0x70) = 0x140000000; /* image_base */
    *(uint32_t*)(buf + 0x78) = 0x1000;  /* section_alignment */
    *(uint32_t*)(buf + 0x7C) = 0x200;    /* file_alignment */
    *(uint16_t*)(buf + 0x80) = 6;       /* major_os_version */
    *(uint16_t*)(buf + 0x82) = 0;       /* minor_os_version */
    *(uint16_t*)(buf + 0x84) = 0;       /* major_image_version */
    *(uint16_t*)(buf + 0x86) = 0;       /* minor_image_version */
    *(uint16_t*)(buf + 0x88) = 6;       /* major_subsystem_version */
    *(uint16_t*)(buf + 0x8A) = 0;       /* minor_subsystem_version */
    *(uint32_t*)(buf + 0x8C) = 0;       /* win32_version_value */
    *(uint32_t*)(buf + 0x90) = 0x2000;  /* size_of_image */
    *(uint32_t*)(buf + 0x94) = 0x200;   /* size_of_headers */
    *(uint32_t*)(buf + 0x98) = 0;       /* checksum */
    *(uint16_t*)(buf + 0x9C) = 1;       /* subsystem */
    *(uint16_t*)(buf + 0x9E) = 0;       /* dll_characteristics */
    *(uint64_t*)(buf + 0xA0) = 0x100000; /* size_of_stack_reserve */
    *(uint64_t*)(buf + 0xA8) = 0x1000;   /* size_of_stack_commit */
    *(uint64_t*)(buf + 0xB0) = 0x100000; /* size_of_heap_reserve */
    *(uint64_t*)(buf + 0xB8) = 0x1000;   /* size_of_heap_commit */
    *(uint32_t*)(buf + 0xC0) = 0;       /* loader_flags */
    *(uint32_t*)(buf + 0xC4) = 16;      /* number_of_rva_and_sizes */
    /* data_directories at 0xC8 (248 = 0xF8) */
    /* Section header at 0x58 + 240 = 0x148 */
    memcpy(buf + 0x148, ".text\0\0\0", 8);
    *(uint32_t*)(buf + 0x150) = 0x200;  /* virtual_size */
    *(uint32_t*)(buf + 0x154) = 0x1000; /* virtual_address */
    *(uint32_t*)(buf + 0x158) = 0x200;   /* size_of_raw_data */
    *(uint32_t*)(buf + 0x15C) = 0x200;  /* pointer_to_raw_data */
    *(uint32_t*)(buf + 0x160) = 0;       /* relocations */
    *(uint32_t*)(buf + 0x164) = 0;       /* linenumbers */
    *(uint16_t*)(buf + 0x168) = 0;       /* number_of_relocations */
    *(uint16_t*)(buf + 0x16A) = 0;       /* number_of_linenumbers */
    *(uint32_t*)(buf + 0x16C) = 0x40000040; /* initialized data, readable */

    /* Section data at 0x200 */
    /* mov eax, 0x1234; ret */
    buf[0x200] = 0xB8;
    buf[0x201] = 0x34;
    buf[0x202] = 0x12;
    buf[0x203] = 0x00;
    buf[0x204] = 0x00;
    buf[0x205] = 0xC3;

    *out_size = total;
    return buf;
}

TEST(pe_map_image) {
    size_t pe_size;
    uint8_t* pe_data = build_minimal_pe64(&pe_size);
    hb_pe_image_t* pe = hb_pe_load(pe_data, pe_size);
    ASSERT(pe != NULL);
    ASSERT(pe->number_of_sections == 1);

    hb_result_t r = hb_pe_map_image(pe, 0);
    ASSERT(r == HB_OK);
    ASSERT(pe->mapped_image != NULL);
    ASSERT(pe->mapped_size >= pe->size_of_image);

    /* Verify section data was copied */
    uint8_t* text = pe->mapped_image + 0x1000;
    ASSERT(text[0] == 0xB8);
    ASSERT(text[1] == 0x34);
    ASSERT(text[5] == 0xC3);

    hb_pe_unload(pe);
    free(pe_data);
    tests_passed++;
}

TEST(pe_map_image_mprotect_failure_returns_error) {
    size_t pe_size;
    uint8_t* pe_data = build_minimal_pe64(&pe_size);
    ASSERT(pe_data != NULL);
    *(uint32_t*)(pe_data + 0x16C) = 0x60000020; /* executable code section */

    hb_pe_image_t* pe = hb_pe_load(pe_data, pe_size);
    ASSERT(pe != NULL);
    setenv("MACRUNNER_HB_TEST_FORCE_PE_MPROTECT_FAIL", "1", 1);
    hb_result_t r = hb_pe_map_image(pe, 0);
    unsetenv("MACRUNNER_HB_TEST_FORCE_PE_MPROTECT_FAIL");
    ASSERT(r == HB_ERR_JIT_FAILED);

    hb_pe_unload(pe);
    free(pe_data);
    tests_passed++;
}

TEST(pe_load_rejects_e_lfanew_overflow) {
    uint8_t pe_data[64];
    memset(pe_data, 0, sizeof(pe_data));
    *(uint16_t*)(pe_data + 0x00) = 0x5A4D;
    *(uint32_t*)(pe_data + 0x3C) = 0xfffffff8u;
    ASSERT(hb_pe_load(pe_data, sizeof(pe_data)) == NULL);
    tests_passed++;
}

TEST(pe_load_rejects_section_table_oob) {
    size_t pe_size;
    uint8_t* pe_data = build_minimal_pe64(&pe_size);
    ASSERT(pe_data != NULL);
    ASSERT(hb_pe_load(pe_data, 0x160) == NULL);
    free(pe_data);
    tests_passed++;
}

TEST(pe_map_rejects_section_raw_oob) {
    size_t pe_size;
    uint8_t* pe_data = build_minimal_pe64(&pe_size);
    ASSERT(pe_data != NULL);
    *(uint32_t*)(pe_data + 0x158) = 0x300; /* size_of_raw_data */
    *(uint32_t*)(pe_data + 0x15C) = 0x300; /* pointer_to_raw_data */

    hb_pe_image_t* pe = hb_pe_load(pe_data, pe_size);
    ASSERT(pe != NULL);
    ASSERT(hb_pe_map_image(pe, 0) == HB_ERR_PE_PARSE);

    hb_pe_unload(pe);
    free(pe_data);
    tests_passed++;
}

TEST(pe_relocation_directory_oob_returns_parse) {
    hb_pe_image_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.mapped_image = calloc(1, 64);
    ASSERT(pe.mapped_image != NULL);
    pe.mapped_size = 64;
    pe.preferred_base = 0x1000;
    pe.data_directory[HB_PE_DD_BASERELOC][0] = 60;
    pe.data_directory[HB_PE_DD_BASERELOC][1] = 16;

    ASSERT(hb_pe_apply_relocations(&pe, 0x2000) == HB_ERR_PE_PARSE);
    free(pe.mapped_image);
    tests_passed++;
}

TEST(pe_import_name_rva_bounds_checked) {
    hb_pe_image_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.mapped_image = calloc(1, 8);
    ASSERT(pe.mapped_image != NULL);
    pe.mapped_size = 8;
    ASSERT(hb_pe_import_dll_name(&pe, 7) != NULL);
    ASSERT(hb_pe_import_dll_name(&pe, 8) == NULL);
    ASSERT(hb_pe_import_func_name(&pe, 5) != NULL);
    ASSERT(hb_pe_import_func_name(&pe, 6) == NULL);
    free(pe.mapped_image);
    tests_passed++;
}

TEST(controlled_execution_mvp) {
    /* End-to-end: allocate raw memory, write instructions, decode+lift+run */
    /* Code: mov eax, 0xABCD; ret */
    uint8_t code[] = {0xB8, 0xCD, 0xAB, 0x00, 0x00, 0xC3};

    /* Allocate a page and copy code */
    size_t page = 4096;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
    flags |= MAP_JIT;
#endif
    void* mem = mmap(NULL, page, PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT(mem != MAP_FAILED);
    memcpy(mem, code, sizeof(code));
    if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, page);
        tests_failed++;
        return;
    }
    __builtin___clear_cache((char*)mem, (char*)mem + page);

    /* Create decoder */
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64,
                                          (const uint8_t*)mem,
                                          sizeof(code),
                                          (uint64_t)(uintptr_t)mem);
    ASSERT(dec != NULL);

    /* Lift function */
    hb_ir_func_t* func = NULL;
    hb_result_t r = hb_lift_func_x64(dec, &func);
    hb_decoder_destroy(dec);
    ASSERT(r == HB_OK);
    ASSERT(func != NULL);

    /* Create context, set up memory and stack */
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = (uint64_t)(uintptr_t)mem;
    ctx->memory = hb_memory_create(0x100000);
    ASSERT(ctx->memory != NULL);
    r = hb_abi_setup_stack(ctx, 65536);
    ASSERT(r == HB_OK);
    /* Push return sentinel so RET has somewhere to go */
    ctx->regs.x64.rsp -= 8;
    r = hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp, 0xFFFF0000);
    ASSERT(r == HB_OK);

    hb_exec_result_t out;
    r = hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out);
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0xABCD);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    munmap(mem, page);
    tests_passed++;
}

TEST(interp_x64_gs_teb_load) {
    /* Windows x64 stack probes read TEB fields via GS, e.g. mov %gs:0x10,%r11. */
    uint8_t code[] = {
        0x65, 0x48, 0x8b, 0x1c, 0x25, 0x10, 0x00, 0x00, 0x00, /* mov %gs:0x10,%rbx */
        0xc3                                                        /* ret */
    };
    hb_decoded_t d;
    uint64_t code_base = (uint64_t)(uintptr_t)code;
    ASSERT(hb_decode_x64(code, sizeof(code), code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op2.is_mem);
    ASSERT(d.op2.mem.segment == 0x65);
    ASSERT(d.op2.mem.disp == 0x10);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    void* teb = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(teb != MAP_FAILED);
    *(uint64_t*)((uint8_t*)teb + 0x10) = 0x123456789abcdef0ULL;

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, code_base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)teb, 4096, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_abi_setup_stack(ctx, 65536) == HB_OK);
    ctx->pc = code_base;
    ctx->gs_base = (uint64_t)(uintptr_t)teb;
    ctx->regs.x64.rsp -= 8;
    ASSERT(hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp, 0xFFFF0000) == HB_OK);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rbx == 0x123456789abcdef0ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    munmap(teb, 4096);
    tests_passed++;
}

/* --- SHIFT TESTS --- */

TEST(jit_shl_basic_full) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);

    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_SHL,
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_imm(65, HB_SIZE_64));

    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ctx->pc = 0x1000;
    hb_exec_result_t out;
    hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);

    ASSERT(ctx->regs.x64.rax == 2);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_shr_cf_flag_full) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);

    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_SHR,
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_imm(1, HB_SIZE_64));

    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ctx->pc = 0x1000;
    hb_exec_result_t out;
    hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);

    materialize_shift_flags(ctx, 1);
    ASSERT(ctx->regs.x64.rax == 0);
    ASSERT(ctx->flags.cf == 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_shl_count_zero_flags_preserved) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);

    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(5, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_SHL,
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_imm(0, HB_SIZE_64));

    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ctx->pc = 0x1000;
    ctx->flags.cf = 1;

    hb_exec_result_t out;
    hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);

    ASSERT(ctx->regs.x64.rax == 5);
    ASSERT(ctx->flags.cf == 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(diff_shl_jit_vs_interp) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);

    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1ULL << 63, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_SHL,
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_imm(1, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* jit = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_context_t* interp = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    jit->pc = 0x1000;
    interp->pc = 0x1000;

    hb_exec_result_t out1, out2;
    hb_runtime_run(jit, func, HB_BACKEND_JIT, &out1);
    hb_runtime_run(interp, func, HB_BACKEND_INTERP, &out2);
    materialize_shift_flags(jit, 1);
    materialize_shift_flags(interp, 1);

    ASSERT(jit->regs.x64.rax == interp->regs.x64.rax);
    ASSERT(jit->flags.cf == interp->flags.cf);
    ASSERT(jit->flags.of == interp->flags.of);
    ASSERT(jit->flags.zf == interp->flags.zf);
    ASSERT(jit->flags.sf == interp->flags.sf);

    hb_context_destroy(jit);
    hb_context_destroy(interp);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(diff_shift_fuzzer_small) {
    uint64_t values[] = {
        0ULL,
        1ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0x7FFFFFFFFFFFFFFFULL
    };

    uint64_t counts[] = {0,1,2,7,31,32,63,64,65};

    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        for (size_t j = 0; j < sizeof(counts)/sizeof(counts[0]); j++) {
            hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
            hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
            hb_ir_cfg_add_block(func->cfg, blk);
            func->cfg->entry = blk;

            hb_ir_builder_t* b = hb_ir_builder_create(func);
            hb_ir_builder_set_block(b, blk);

            hb_ir_emit_mov(b,
                hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                hb_ir_imm(values[i], HB_SIZE_64));

            hb_ir_emit_binop(b, HB_IR_SHL,
                hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                hb_ir_imm(counts[j], HB_SIZE_64));

            hb_ir_builder_destroy(b);

            hb_context_t* jit = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
            hb_context_t* interp = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
            jit->pc = 0x1000;
            interp->pc = 0x1000;

            hb_exec_result_t o1, o2;
            hb_runtime_run(jit, func, HB_BACKEND_JIT, &o1);
            hb_runtime_run(interp, func, HB_BACKEND_INTERP, &o2);
            materialize_shift_flags(jit, counts[j]);
            materialize_shift_flags(interp, counts[j]);

            ASSERT(jit->regs.x64.rax == interp->regs.x64.rax);
            ASSERT(jit->flags.cf == interp->flags.cf);
            ASSERT(jit->flags.of == interp->flags.of);
            ASSERT(jit->flags.zf == interp->flags.zf);
            ASSERT(jit->flags.sf == interp->flags.sf);

            hb_context_destroy(jit);
            hb_context_destroy(interp);
            hb_ir_func_destroy(func);
        }
    }

    tests_passed++;
}

TEST(diff_shift_fuzzer_final_boss) {
    srand(0);
    for (int i = 0; i < 50; i++) {
        uint64_t val = ((uint64_t)rand() << 32) ^ rand();
        uint64_t count = rand() % 130;

        hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
        hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
        hb_ir_cfg_add_block(func->cfg, blk);
        func->cfg->entry = blk;

        hb_ir_builder_t* b = hb_ir_builder_create(func);
        hb_ir_builder_set_block(b, blk);

        hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(val, HB_SIZE_64));

        int op = rand() % 3;
        hb_ir_op_t opc = (op == 0) ? HB_IR_SHL : (op == 1) ? HB_IR_SHR : HB_IR_SAR;

        hb_ir_emit_binop(b, opc,
            hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
            hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
            hb_ir_imm(count, HB_SIZE_64));

        hb_ir_builder_destroy(b);

        hb_context_t* jit = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
        hb_context_t* interp = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
        jit->pc = 0x1000;
        interp->pc = 0x1000;

        hb_exec_result_t o1, o2;
        hb_runtime_run(jit, func, HB_BACKEND_JIT, &o1);
        hb_runtime_run(interp, func, HB_BACKEND_INTERP, &o2);
        materialize_shift_flags(jit, count);
        materialize_shift_flags(interp, count);

        ASSERT(jit->regs.x64.rax == interp->regs.x64.rax);
        ASSERT(jit->flags.cf == interp->flags.cf);
        ASSERT(jit->flags.of == interp->flags.of);
        ASSERT(jit->flags.zf == interp->flags.zf);
        ASSERT(jit->flags.sf == interp->flags.sf);

        hb_context_destroy(jit);
        hb_context_destroy(interp);
        hb_ir_func_destroy(func);
    }

    tests_passed++;
}

TEST(decode_rol_rcx_imm8_calc) {
    uint8_t code[] = {0x48, 0xc1, 0xc1, 0x0d}; /* rol $13, %rcx */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ROL);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_imm && d.op2.imm == 13);
    tests_passed++;
}

TEST(decode_shl_al_imm8_notepadpp) {
    uint8_t code[] = {0xc0, 0xe0, 0x02}; /* shl al, 2 */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1401db982, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHL);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 2);
    ASSERT(d.op2.size == 1);
    tests_passed++;
}

TEST(decode_ror_bl_cl_byte_group_d2) {
    uint8_t code[] = {0xd2, 0xcb}; /* ror bl, cl */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ROR);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX);
    ASSERT(d.op2.size == 1);
    tests_passed++;
}

TEST(decode_prefetchw_notepadpp_hint_nop) {
    uint8_t code[] = {0x0f, 0x0d, 0x4b, 0x14}; /* prefetchw 0x14(%rbx) */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x140408902, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.mem.disp == 0x14);
    ASSERT(d.op1.size == 1);
    tests_passed++;
}

TEST(decode_prefetcht0_hint_nop) {
    uint8_t code[] = {0x0f, 0x18, 0x08}; /* prefetcht0 (%rax) */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 1);
    tests_passed++;
}

TEST(decode_x86_nop_hint_family) {
    hb_decoded_t d;
    uint8_t nop[] = {0x90};
    uint8_t prefixed_nop[] = {0x66, 0x90}; /* ntdll!LdrInitializeThunk trigger */
    uint8_t pause_nop[] = {0xf3, 0x90};
    uint8_t nopl[] = {0x0f, 0x1f, 0x00}; /* nopl (%eax) */
    uint8_t prefetchw[] = {0x0f, 0x0d, 0x4b, 0x14}; /* prefetchw 0x14(%ebx) */
    uint8_t prefetcht0[] = {0x0f, 0x18, 0x08}; /* prefetcht0 (%eax) */

    ASSERT(hb_decode_x86(nop, sizeof(nop), 0x7bdb0f20, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 1);

    ASSERT(hb_decode_x86(prefixed_nop, sizeof(prefixed_nop), 0x7bdb0f20, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x86(pause_nop, sizeof(pause_nop), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 2);

    ASSERT(hb_decode_x86(nopl, sizeof(nopl), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    ASSERT(hb_decode_x86(prefetchw, sizeof(prefetchw), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.mem.disp == 0x14);
    ASSERT(d.op1.size == 1);

    ASSERT(hb_decode_x86(prefetcht0, sizeof(prefetcht0), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 1);
    tests_passed++;
}

TEST(decode_x86_f6_f7_group3_family) {
    hb_decoded_t d;
    uint8_t testb_mem[] = {0xf6, 0x83, 0xcb, 0x0f, 0x00, 0x00, 0x40};
    uint8_t testl_mem[] = {0xf7, 0x00, 0x78, 0x56, 0x34, 0x12};
    uint8_t notb_al[] = {0xf6, 0xd0};
    uint8_t mulb_bl[] = {0xf6, 0xe3};

    ASSERT(hb_decode_x86(testb_mem, sizeof(testb_mem), 0x7bd8af4e, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST);
    ASSERT(d.len == 7);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.mem.disp == 0xfcb);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x40);

    ASSERT(hb_decode_x86(testl_mem, sizeof(testl_mem), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST);
    ASSERT(d.len == 6);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x12345678);

    ASSERT(hb_decode_x86(notb_al, sizeof(notb_al), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOT);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 1);

    ASSERT(hb_decode_x86(mulb_bl, sizeof(mulb_bl), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MUL);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX);
    ASSERT(d.op1.size == 1);
    tests_passed++;
}

TEST(interp_x86_f6_group3_test_and_mul) {
    const uint32_t code_base = 0x00403000u;
    uint8_t testb_code[] = {0xf6, 0x83, 0xcb, 0x0f, 0x00, 0x00, 0x40};
    uint8_t mulb_code[] = {0xf6, 0xe3};

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, testb_code, sizeof(testb_code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, 0x00001000u, 8192,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u8(ctx->memory, 0x00001fcb, 0x40) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = 0x00001000u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(testb_code));
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X86, mulb_code, sizeof(mulb_code), code_base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 3;
    ctx->regs.x86.ebx = 5;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((ctx->regs.x86.eax & 0xffffu) == 15u);
    ASSERT(ctx->flags.cf == false);
    ASSERT(ctx->flags.of == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_fs_segment_load_uses_teb32_base) {
    const uint32_t code_base = 0x00404000u;
    const uint32_t teb32 = 0x00002000u;
    uint8_t code[] = {0x64, 0x8b, 0x1d, 0x18, 0x00, 0x00, 0x00}; /* mov ebx, fs:[0x18] */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    hb_decoded_t d;
    ASSERT(dec != NULL);
    ASSERT(hb_decode_x86(code, sizeof(code), code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op2.is_mem && d.op2.mem.segment == 0x64);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, teb32, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, teb32 + 0x18, 0x1234abcdU) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->fs_base = teb32;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.ebx == 0x1234abcdU);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_mem_imm_store_and_push_mem) {
    const uint32_t code_base = 0x00405000u;
    const uint32_t stack_base = 0x00610000u;
    const uint32_t stack_top = stack_base + 0x1000u;
    uint8_t mov_mem_imm[] = {0xc7, 0x45, 0xb8, 0x78, 0x56, 0x34, 0x12}; /* mov [ebp-0x48], imm32 */
    uint8_t mov_mem_imm16[] = {0x66, 0xc7, 0x45, 0xbc, 0x34, 0x12}; /* mov word [ebp-0x44], 0x1234 */
    uint8_t push_mem[] = {0xff, 0x74, 0x24, 0x04}; /* push dword [esp+4] */
    uint32_t value = 0;
    uint16_t value16 = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, mov_mem_imm, sizeof(mov_mem_imm), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebp = stack_top;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u32(ctx->memory, stack_top - 0x48, &value) == HB_OK);
    ASSERT(value == 0x12345678U);
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X86, mov_mem_imm16, sizeof(mov_mem_imm16), code_base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebp = stack_top;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u16(ctx->memory, stack_top - 0x44, &value16) == HB_OK);
    ASSERT(value16 == 0x1234U);
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X86, push_mem, sizeof(push_mem), code_base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = stack_top - 0x20;
    ASSERT(hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp + 4, 0xaabbccddU) == HB_OK);
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.esp == stack_top - 0x24);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &value) == HB_OK);
    ASSERT(value == 0xaabbccddU);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_pop_rm32_family) {
    hb_decoded_t d;
    uint8_t pop_fs0[] = {0x64, 0x8f, 0x05, 0x00, 0x00, 0x00, 0x00}; /* pop dword fs:[0] */
    uint8_t pop_mem[] = {0x8f, 0x45, 0xfc};                         /* pop dword [ebp-4] */
    uint8_t pop_reg[] = {0x8f, 0xc3};                                /* pop ebx via r/m32 */

    ASSERT(hb_decode_x86(pop_fs0, sizeof(pop_fs0), 0x7bdb067d, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP);
    ASSERT(d.len == 7);
    ASSERT(d.stack_delta == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0);
    ASSERT(d.op1.mem.segment == 0x64);
    ASSERT(d.op1.size == 4);

    ASSERT(hb_decode_x86(pop_mem, sizeof(pop_mem), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBP);
    ASSERT(d.op1.mem.disp == -4);
    ASSERT(d.op1.size == 4);

    ASSERT(hb_decode_x86(pop_reg, sizeof(pop_reg), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POP);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX);
    ASSERT(d.op1.size == 4);
    tests_passed++;
}

TEST(interp_x86_pop_rm32_fs_seh_restore) {
    const uint32_t code_base = 0x0040a000u;
    const uint32_t teb32 = 0x00002000u;
    const uint32_t stack_base = 0x00620000u;
    uint8_t code[] = {
        0x64, 0x8f, 0x05, 0x00, 0x00, 0x00, 0x00, /* pop dword fs:[0] */
        0x8f, 0x45, 0xfc                          /* pop dword [ebp-4] */
    };
    uint32_t value = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, teb32, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = stack_base + 0x800;
    ctx->regs.x86.ebp = stack_base + 0x400;
    ctx->fs_base = teb32;
    ASSERT(hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, 0x11112222u) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp + 4, 0x33334444u) == HB_OK);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.esp == stack_base + 0x808);
    ASSERT(hb_memory_read_u32(ctx->memory, teb32, &value) == HB_OK);
    ASSERT(value == 0x11112222u);
    ASSERT(hb_memory_read_u32(ctx->memory, ctx->regs.x86.ebp - 4, &value) == HB_OK);
    ASSERT(value == 0x33334444u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_ff_indirect_branch_family) {
    hb_decoded_t d;
    uint8_t call_edx[] = {0xff, 0xd2};                    /* call edx */
    uint8_t jmp_abs[] = {0xff, 0x25, 0x00, 0xb0, 0x00, 0x00};  /* jmp dword [0xb000] */
    uint8_t push_abs[] = {0xff, 0x35, 0x04, 0xb0, 0x00, 0x00}; /* push dword [0xb004] */

    ASSERT(hb_decode_x86(call_edx, sizeof(call_edx), 0x7bdb00fa, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CALL);
    ASSERT(d.is_call);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 4);

    ASSERT(hb_decode_x86(jmp_abs, sizeof(jmp_abs), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_JMP);
    ASSERT(d.is_branch);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0xb000);
    ASSERT(d.op1.size == 4);

    ASSERT(hb_decode_x86(push_abs, sizeof(push_abs), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSH);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0xb004);
    ASSERT(d.op1.size == 4);
    tests_passed++;
}

TEST(interp_x86_ff_indirect_call_jmp_targets) {
    const uint32_t call_base = 0x0040b000u;
    const uint32_t jmp_base = 0x0040b100u;
    const uint32_t call_target = 0x0040c000u;
    const uint32_t jmp_target = 0x0040d000u;
    const uint32_t data_base = 0x0000b000u;
    const uint32_t stack_top = 0x00630000u;
    uint8_t call_code[] = {
        0xba, 0x00, 0xc0, 0x40, 0x00, /* mov edx, 0x0040c000 */
        0xff, 0xd2                    /* call edx */
    };
    uint8_t jmp_code[] = {
        0xff, 0x25, 0x00, 0xb0, 0x00, 0x00 /* jmp dword [0xb000] */
    };
    uint32_t ret = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, call_code, sizeof(call_code), call_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, call_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_top - 4096, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = call_base;
    ctx->regs.x86.eip = call_base;
    ctx->regs.x86.esp = stack_top;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == call_target);
    ASSERT(ctx->regs.x86.eip == call_target);
    ASSERT(ctx->regs.x86.esp == stack_top - 4);
    ASSERT(hb_memory_read_u32(ctx->memory, stack_top - 4, &ret) == HB_OK);
    ASSERT(ret == call_base + sizeof(call_code));
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X86, jmp_code, sizeof(jmp_code), jmp_base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base, jmp_target) == HB_OK);
    ctx->pc = jmp_base;
    ctx->regs.x86.eip = jmp_base;
    ctx->regs.x86.esp = stack_top;

    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == jmp_target);
    ASSERT(ctx->regs.x86.eip == jmp_target);
    ASSERT(ctx->regs.x86.esp == stack_top);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_scalar_move_family) {
    hb_decoded_t d;
    uint8_t movsd_load[] = {0xf2, 0x0f, 0x10, 0x05, 0x58, 0x22, 0xc7, 0x7b};
    uint8_t movsd_store[] = {0xf2, 0x0f, 0x11, 0x85, 0x18, 0xff, 0xff, 0xff};
    uint8_t movss_load[] = {0xf3, 0x0f, 0x10, 0x0b};
    uint8_t movss_store[] = {0xf3, 0x0f, 0x11, 0x0b};

    ASSERT(hb_decode_x86(movsd_load, sizeof(movsd_load), 0x7bd8afcb, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 8);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1 && d.op2.mem.disp == 0x7bc72258);
    ASSERT(d.op2.size == 8);

    ASSERT(hb_decode_x86(movsd_store, sizeof(movsd_store), 0x7bd8afd3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBP && d.op1.mem.disp == -0xe8);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 8);

    ASSERT(hb_decode_x86(movss_load, sizeof(movss_load), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1 && d.op1.size == 4);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RBX && d.op2.size == 4);

    ASSERT(hb_decode_x86(movss_store, sizeof(movss_store), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 4);
    tests_passed++;
}

TEST(interp_x86_sse_movsd_load_zeroes_upper_store_low64) {
    const uint32_t code_base = 0x0040e000u;
    const uint32_t data_base = 0x0000c000u;
    uint8_t code[] = {
        0xf2, 0x0f, 0x10, 0x05, 0x00, 0xc0, 0x00, 0x00, /* movsd xmm0, [0xc000] */
        0xf2, 0x0f, 0x11, 0x85, 0x18, 0xff, 0xff, 0xff  /* movsd [ebp-0xe8], xmm0 */
    };
    uint64_t src = 0x1122334455667788ULL;
    uint64_t stored = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg && func->cfg->entry && func->cfg->entry->instr_count >= 1);
    ASSERT(func->cfg->entry->instrs[0].op == HB_IR_LOAD);
    ASSERT(func->cfg->entry->instrs[0].zero_upper);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base, &src, sizeof(src)) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebp = data_base + 0x100;
    ctx->regs.x86.xmm[0][0] = 0xaabbccddeeff0011ULL;
    ctx->regs.x86.xmm[0][1] = 0x8877665544332211ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == src);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0);
    ASSERT(hb_memory_read(ctx->memory, data_base + 0x18, &stored, sizeof(stored)) == HB_OK);
    ASSERT(stored == src);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_sse_movss_mem_load_zeroes_upper_reg_move_preserves_upper) {
    const uint32_t code_base = 0x0040e100u;
    const uint32_t data_base = 0x0000c000u;
    uint8_t code[] = {
        0xf3, 0x0f, 0x10, 0x05, 0x00, 0xc0, 0x00, 0x00, /* movss xmm0, [0xc000] */
        0xf3, 0x0f, 0x10, 0xc8                          /* movss xmm1, xmm0 */
    };
    uint32_t src = 0x11223344u;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg && func->cfg->entry && func->cfg->entry->instr_count >= 2);
    ASSERT(func->cfg->entry->instrs[0].op == HB_IR_LOAD);
    ASSERT(func->cfg->entry->instrs[0].zero_upper);
    ASSERT(!func->cfg->entry->instrs[1].zero_upper);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base, src) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.xmm[0][0] = 0xaabbccddeeff0011ULL;
    ctx->regs.x86.xmm[0][1] = 0x8877665544332211ULL;
    ctx->regs.x86.xmm[1][0] = 0x0123456789abcdefULL;
    ctx->regs.x86.xmm[1][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == src);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0);
    ASSERT(ctx->regs.x86.xmm[1][0] == 0x0123456711223344ULL);
    ASSERT(ctx->regs.x86.xmm[1][1] == 0xfedcba9876543210ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_lift_x86_notepadpp_scalar_sse_boundary) {
    const uint32_t code_base = 0x005455d9u;
    uint8_t code[] = {
        0x89, 0x45, 0xe4,                         /* mov [ebp-0x1c], eax */
        0x0f, 0x57, 0xc0,                         /* xorps xmm0, xmm0 */
        0x83, 0xc0, 0x08,                         /* add eax, 8 */
        0x83, 0xc4, 0x04,                         /* add esp, 4 */
        0x89, 0x45, 0xe8,                         /* mov [ebp-0x18], eax */
        0x0f, 0x11, 0x00,                         /* movups [eax], xmm0 */
        0xc7, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00, /* mov dword [eax+0x10], 0 */
        0xc7, 0x40, 0x14, 0x00, 0x00, 0x00, 0x00, /* mov dword [eax+0x14], 0 */
        0x0f, 0x10, 0x07,                         /* movups xmm0, [edi] */
        0x0f, 0x11, 0x00,                         /* movups [eax], xmm0 */
        0xf3, 0x0f, 0x7e, 0x47, 0x10,             /* movq xmm0, [edi+0x10] */
        0x66, 0x0f, 0xd6, 0x40, 0x10,             /* movq [eax+0x10], xmm0 */
        0xc6, 0x45, 0xfc, 0x01,                   /* mov byte [ebp-4], 1 */
        0xc7, 0x47, 0x10, 0x00, 0x00, 0x00, 0x00, /* mov dword [edi+0x10], 0 */
        0xc7, 0x47, 0x14, 0x0f, 0x00, 0x00, 0x00, /* mov dword [edi+0x14], 0xf */
        0xc6, 0x07, 0x00,                         /* mov byte [edi], 0 */
        0xc7, 0x40, 0x18, 0x00, 0x00, 0x00, 0x00, /* mov dword [eax+0x18], 0 */
        0xc7, 0x45, 0xfc, 0x03, 0x00, 0x00, 0x00, /* mov dword [ebp-4], 3 */
        0x8b, 0x7d, 0xec,                         /* mov edi, [ebp-0x14] */
        0x8b, 0x47, 0x08,                         /* mov eax, [edi+8] */
        0x40,                                     /* inc eax */
        0x66, 0x0f, 0x6e, 0xc0,                   /* movd xmm0, eax */
        0xf3, 0x0f, 0xe6, 0xc0,                   /* cvtdq2pd xmm0, xmm0 */
        0xc1, 0xe8, 0x1f,                         /* shr eax, 0x1f */
        0xf2, 0x0f, 0x58, 0x04, 0xc5, 0xf0, 0x71, 0x85, 0x00,
        0x8b, 0x47, 0x1c,                         /* mov eax, [edi+0x1c] */
        0x66, 0x0f, 0x5a, 0xc8,                   /* cvtpd2ps xmm1, xmm0 */
        0x66, 0x0f, 0x6e, 0xc0,                   /* movd xmm0, eax */
        0xf3, 0x0f, 0xe6, 0xc0,                   /* cvtdq2pd xmm0, xmm0 */
        0xc1, 0xe8, 0x1f,                         /* shr eax, 0x1f */
        0xf2, 0x0f, 0x58, 0x04, 0xc5, 0xf0, 0x71, 0x85, 0x00,
        0x66, 0x0f, 0x5a, 0xc0,                   /* cvtpd2ps xmm0, xmm0 */
        0xf3, 0x0f, 0x5e, 0xc8,                   /* divss xmm1, xmm0 */
        0x0f, 0x2f, 0x0f,                         /* comiss xmm1, [edi] */
        0x76, 0x24                                /* jbe +0x24 */
    };
    hb_decoded_t d;

    ASSERT(hb_decode_x86(code + 94, sizeof(code) - 94, code_base + 94, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTDQ2PD);
    ASSERT(hb_decode_x86(code + 101, sizeof(code) - 101, code_base + 101, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADDSD);
    ASSERT(hb_decode_x86(code + 113, sizeof(code) - 113, code_base + 113, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTPD2PS);
    ASSERT(hb_decode_x86(code + 141, sizeof(code) - 141, code_base + 141, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_DIVSS);
    ASSERT(hb_decode_x86(code + 145, sizeof(code) - 145, code_base + 145, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_COMISS);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RDI && d.op2.size == 4);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(!func->has_unsupported);

    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_lift_x86_notepadpp_x87_comisd_lahf_boundary) {
    const uint32_t code_base = 0x00781f0bu;
    uint8_t code[] = {
        0xf2, 0x0f, 0x10, 0x45, 0x08, /* movsd xmm0, [ebp+8] */
        0x83, 0xc4, 0x08,             /* add esp, 8 */
        0xdd, 0x55, 0xf0,             /* fst qword [ebp-0x10] */
        0xdd, 0x5d, 0xf8,             /* fstp qword [ebp-8] */
        0xf2, 0x0f, 0x10, 0x4d, 0xf8, /* movsd xmm1, [ebp-8] */
        0x66, 0x0f, 0x2e, 0xc8,       /* ucomisd xmm1, xmm0 */
        0x9f,                         /* lahf */
        0xf6, 0xc4, 0x44              /* test ah, 0x44 */
    };
    hb_decoded_t d;

    ASSERT(hb_decode_x86(code + 23, sizeof(code) - 23, code_base + 23, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LAHF);
    ASSERT(d.reads_flags);
    ASSERT(d.len == 1);

    uint8_t sahf[] = {0x9e};
    ASSERT(hb_decode_x86(sahf, sizeof(sahf), code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SAHF);
    ASSERT(d.writes_flags);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(!func->has_unsupported);

    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_scalar_sse_convert_arith_compare_family) {
    const uint32_t convert_base = 0x0040f000u;
    uint8_t convert_code[] = {
        0xf3, 0x0f, 0xe6, 0xc0, /* cvtdq2pd xmm0, xmm0 */
        0x66, 0x0f, 0x5a, 0xc8  /* cvtpd2ps xmm1, xmm0 */
    };
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, convert_code, sizeof(convert_code), convert_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, convert_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, convert_base, convert_code, sizeof(convert_code)) == HB_OK);
    ctx->pc = convert_base;
    ctx->regs.x86.eip = convert_base;
    {
        int32_t dwords[4] = {4, -2, 0, 0};
        memcpy(ctx->regs.x86.xmm[0], dwords, sizeof(dwords));
    }

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x86.xmm[1][0] == test_float_bits(4.0f));
    ASSERT((uint32_t)(ctx->regs.x86.xmm[1][0] >> 32) == test_float_bits(-2.0f));
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t arith_code[] = {
        0xf2, 0x0f, 0x58, 0xc1, /* addsd xmm0, xmm1 */
        0xf3, 0x0f, 0x5e, 0xd3, /* divss xmm2, xmm3 */
        0x0f, 0x2f, 0xe5        /* comiss xmm4, xmm5 */
    };
    const uint32_t arith_base = 0x00410000u;
    dec = hb_decoder_create(HB_ARCH_X86, arith_code, sizeof(arith_code), arith_base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, arith_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, arith_base, arith_code, sizeof(arith_code)) == HB_OK);
    ctx->pc = arith_base;
    ctx->regs.x86.eip = arith_base;
    ctx->regs.x86.xmm[0][0] = test_double_bits(2.5);
    ctx->regs.x86.xmm[1][0] = test_double_bits(1.25);
    ctx->regs.x86.xmm[2][0] = test_float_bits(12.0f);
    ctx->regs.x86.xmm[3][0] = test_float_bits(3.0f);
    ctx->regs.x86.xmm[4][0] = test_float_bits(5.0f);
    ctx->regs.x86.xmm[5][0] = test_float_bits(5.0f);

    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == test_double_bits(3.75));
    ASSERT((uint32_t)ctx->regs.x86.xmm[2][0] == test_float_bits(4.0f));
    ASSERT(ctx->flags.zf == true);
    ASSERT(ctx->flags.cf == false);
    ASSERT(ctx->flags.pf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_movlps_low_qword_family_rsaenh) {
    const uint32_t code_base = 0x0040e200u;
    const uint32_t data_base = 0x0000c000u;
    uint8_t code[] = {
        0x0f, 0x13, 0x07,             /* movlps [edi], xmm0 */
        0x66, 0x0f, 0x13, 0x4f, 0x08, /* movlpd [edi+8], xmm1 */
        0x0f, 0x12, 0x10              /* movlps xmm2, [eax] */
    };
    hb_decoded_t d;

    ASSERT(hb_decode_x86(code, 3, code_base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 8 && d.op1.mem.base == HB_REG_RDI);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 8);
    ASSERT(hb_decode_x86(code + 3, 5, code_base + 3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 8 && d.op1.mem.base == HB_REG_RDI && d.op1.mem.disp == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 8);
    ASSERT(hb_decode_x86(code + 8, 3, code_base + 8, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM2 && d.op1.size == 8);
    ASSERT(d.op2.is_mem && d.op2.size == 8 && d.op2.mem.base == HB_REG_RAX);

    uint8_t reg_store[] = {0x0f, 0x13, 0xc0};
    uint8_t reg_load[] = {0x66, 0x0f, 0x12, 0xc0};
    ASSERT(hb_decode_x86(reg_store, sizeof(reg_store), code_base, &d) == HB_ERR_UNSUPPORTED_OPCODE);
    ASSERT(hb_decode_x86(reg_load, sizeof(reg_load), code_base, &d) == HB_ERR_UNSUPPORTED_OPCODE);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg && func->cfg->entry && func->cfg->entry->instr_count >= 3);
    ASSERT(func->cfg->entry->instrs[2].op == HB_IR_LOAD);
    ASSERT(!func->cfg->entry->instrs[2].zero_upper);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    uint8_t backing[64];
    memset(backing, 0xaa, sizeof(backing));
    uint64_t src = 0x1020304050607080ULL;
    memcpy(backing + 0x20, &src, sizeof(src));
    ASSERT(hb_memory_write(ctx->memory, data_base, backing, sizeof(backing)) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.edi = data_base;
    ctx->regs.x86.eax = data_base + 0x20;
    ctx->regs.x86.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x86.xmm[0][1] = 0x99aabbccddeeff00ULL;
    ctx->regs.x86.xmm[1][0] = 0x8877665544332211ULL;
    ctx->regs.x86.xmm[1][1] = 0x0102030405060708ULL;
    ctx->regs.x86.xmm[2][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x86.xmm[2][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint64_t got = 0;
    ASSERT(hb_memory_read(ctx->memory, data_base, &got, sizeof(got)) == HB_OK);
    ASSERT(got == 0x1122334455667788ULL);
    ASSERT(hb_memory_read(ctx->memory, data_base + 8, &got, sizeof(got)) == HB_OK);
    ASSERT(got == 0x8877665544332211ULL);
    ASSERT(hb_memory_read(ctx->memory, data_base + 16, &got, sizeof(got)) == HB_OK);
    ASSERT(got == 0xaaaaaaaaaaaaaaaaULL);
    ASSERT(ctx->regs.x86.xmm[2][0] == src);
    ASSERT(ctx->regs.x86.xmm[2][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_group1_immediate_family) {
    hb_decoded_t d;
    uint8_t and_esp[] = {0x83, 0xe4, 0xf8}; /* and esp, -8 */
    uint8_t or_m8[] = {0x80, 0x0b, 0x7f};   /* or byte [ebx], 0x7f */
    uint8_t adc_eax[] = {0x83, 0xd0, 0x01}; /* adc eax, 1 */
    uint8_t sbb_eax[] = {0x83, 0xd8, 0x01}; /* sbb eax, 1 */
    uint8_t xor_eax[] = {0x81, 0xf0, 0x78, 0x56, 0x34, 0x12}; /* xor eax, imm32 */
    uint8_t cmp_bx[] = {0x66, 0x83, 0xfb, 0xff}; /* cmp bx, -1 */
    uint8_t xor_m16[] = {0x66, 0x81, 0x73, 0x02, 0x34, 0x12}; /* xor word [ebx+2], 0x1234 */

    ASSERT(hb_decode_x86(and_esp, sizeof(and_esp), 0x7bd7eba8, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_AND);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RSP);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_imm && d.op2.imm == -8);
    ASSERT(d.op2.size == 4);

    ASSERT(hb_decode_x86(or_m8, sizeof(or_m8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_OR);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x7f);

    ASSERT(hb_decode_x86(adc_eax, sizeof(adc_eax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADC);
    ASSERT(hb_decode_x86(sbb_eax, sizeof(sbb_eax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SBB);
    ASSERT(hb_decode_x86(xor_eax, sizeof(xor_eax), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XOR);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x12345678);

    ASSERT(hb_decode_x86(cmp_bx, sizeof(cmp_bx), 0x7bd7fb77, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMP);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == -1 && d.op2.size == 2);

    ASSERT(hb_decode_x86(xor_m16, sizeof(xor_m16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XOR);
    ASSERT(d.len == 6);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX && d.op1.mem.disp == 2);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x1234 && d.op2.size == 2);
    tests_passed++;
}

TEST(interp_x86_group1_and_esp_alignment) {
    const uint32_t code_base = 0x00406000u;
    uint8_t code[] = {0x83, 0xe4, 0xf8}; /* and esp, -8 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = 0x00100fffu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.esp == 0x00100ff8u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_group1_operand16_cmp) {
    const uint32_t code_base = 0x00406800u;
    uint8_t code[] = {0x66, 0x83, 0xfb, 0xff}; /* cmp bx, -1 */

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = 0x1234ffffu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF | HB_FLAG_BIT_CF) == HB_OK);
    ASSERT(ctx->flags.zf == true);
    ASSERT(ctx->flags.cf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_alu_modrm_operand16_family) {
    hb_decoded_t d;
    uint8_t add_r16_rm16[] = {0x66, 0x03, 0x48, 0x02}; /* add cx, [eax+2] */
    uint8_t or_r16_rm16[]  = {0x66, 0x0b, 0xc1};       /* or ax, cx */
    uint8_t adc_r16_rm16[] = {0x66, 0x13, 0xd1};       /* adc dx, cx */
    uint8_t sbb_r16_rm16[] = {0x66, 0x1b, 0xd1};       /* sbb dx, cx */
    uint8_t and_r16_rm16[] = {0x66, 0x23, 0xd8};       /* and bx, ax */
    uint8_t sub_r16_rm16[] = {0x66, 0x2b, 0x53, 0xfc}; /* sub dx, [ebx-4] */
    uint8_t xor_rm16_r16[] = {0x66, 0x31, 0xc8};       /* xor ax, cx */
    uint8_t cmp_rm16_r16[] = {0x66, 0x39, 0xd8};       /* cmp ax, bx */
    uint8_t lock_and_rm32_r32[] = {0xf0, 0x21, 0x08};  /* lock and [eax], ecx */
    uint8_t lock_or_rm32_imm8[] = {0xf0, 0x83, 0x08, 0x7f}; /* lock or [eax], 0x7f */
    uint8_t lock_neg_rm32[] = {0xf0, 0xf7, 0x18};      /* lock neg [eax] */

    ASSERT(hb_decode_x86(add_r16_rm16, sizeof(add_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RAX && d.op2.mem.disp == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(or_r16_rm16, sizeof(or_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_OR);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.size == 2);

    ASSERT(hb_decode_x86(adc_r16_rm16, sizeof(adc_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADC && d.op1.reg == HB_REG_RDX && d.op2.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(sbb_r16_rm16, sizeof(sbb_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SBB && d.op1.reg == HB_REG_RDX && d.op2.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(and_r16_rm16, sizeof(and_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_AND && d.op1.reg == HB_REG_RBX && d.op2.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(sub_r16_rm16, sizeof(sub_r16_rm16), 0x7bd8342b, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SUB);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RBX && d.op2.mem.disp == -4 && d.op2.size == 2);

    ASSERT(hb_decode_x86(xor_rm16_r16, sizeof(xor_rm16_r16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XOR && d.op1.reg == HB_REG_RAX && d.op2.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(cmp_rm16_r16, sizeof(cmp_rm16_r16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMP && d.op1.reg == HB_REG_RAX && d.op2.reg == HB_REG_RBX);
    ASSERT(d.op1.size == 2 && d.op2.size == 2);

    ASSERT(hb_decode_x86(lock_and_rm32_r32, sizeof(lock_and_rm32_r32), 0x7bdb2ce5, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_AND);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.size == 4);

    ASSERT(hb_decode_x86(lock_or_rm32_imm8, sizeof(lock_or_rm32_imm8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_OR);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x7f);

    ASSERT(hb_decode_x86(lock_neg_rm32, sizeof(lock_neg_rm32), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NEG);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX && d.op1.size == 4);

    tests_passed++;
}

TEST(decode_x86_test_operand16_family) {
    hb_decoded_t d;
    uint8_t test_r16_r16[] = {0x66, 0x85, 0xff};       /* test di, di */
    uint8_t test_rm8_r8[] = {0x84, 0xc9};              /* test cl, cl */
    uint8_t test_ax_imm16[] = {0x66, 0xa9, 0x34, 0x12}; /* test ax, 0x1234 */
    uint8_t test_eax_imm32[] = {0xa9, 0x78, 0x56, 0x34, 0x12};

    ASSERT(hb_decode_x86(test_r16_r16, sizeof(test_r16_r16), 0x7bd7e1fa, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST && d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDI && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDI && d.op2.size == 2);

    ASSERT(hb_decode_x86(test_rm8_r8, sizeof(test_rm8_r8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST && d.op1.size == 1 && d.op2.size == 1);

    ASSERT(hb_decode_x86(test_ax_imm16, sizeof(test_ax_imm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x1234 && d.op2.size == 2);

    ASSERT(hb_decode_x86(test_eax_imm32, sizeof(test_eax_imm32), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x12345678 && d.op2.size == 4);

    tests_passed++;
}

TEST(decode_x86_bswap_family) {
    hb_decoded_t d;
    uint8_t bswap_eax[] = {0x0f, 0xc8}; /* bswap eax */
    uint8_t bswap_edi[] = {0x0f, 0xcf}; /* bswap edi */

    ASSERT(hb_decode_x86(bswap_eax, sizeof(bswap_eax), 0x7bdaee73, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSWAP);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(!d.writes_flags);

    ASSERT(hb_decode_x86(bswap_edi, sizeof(bswap_edi), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSWAP);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDI && d.op1.size == 4);
    ASSERT(!d.writes_flags);

    tests_passed++;
}

TEST(decode_x86_byte_shift_rotate_family) {
    hb_decoded_t d;
    uint8_t shl_dl_imm[] = {0xc0, 0xe2, 0x04};       /* shl dl, 4 */
    uint8_t shr_mem8_one[] = {0xd0, 0x6e, 0x03};     /* shr byte ptr [esi+3], 1 */
    uint8_t ror_bl_cl[] = {0xd2, 0xcb};              /* ror bl, cl */
    uint8_t shl_ax_one[] = {0x66, 0xd1, 0xe0};       /* shl ax, 1 */

    ASSERT(hb_decode_x86(shl_dl_imm, sizeof(shl_dl_imm), 0x7bd9f34d, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHL && d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 4 && d.op2.size == 1);

    ASSERT(hb_decode_x86(shr_mem8_one, sizeof(shr_mem8_one), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHR && d.len == 3);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RSI && d.op1.mem.disp == 3 && d.op1.size == 1);
    ASSERT(d.op2.is_imm && d.op2.imm == 1);

    ASSERT(hb_decode_x86(ror_bl_cl, sizeof(ror_bl_cl), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ROR && d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX && d.op1.size == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.size == 1);

    ASSERT(hb_decode_x86(shl_ax_one, sizeof(shl_ax_one), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHL && d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);

    tests_passed++;
}

TEST(decode_x86_double_shift_family) {
    hb_decoded_t d;
    uint8_t shld_imm[] = {0x0f, 0xa4, 0xd0, 0x04}; /* shld eax, edx, 4 */
    uint8_t shld_cl[] = {0x0f, 0xa5, 0xd0};        /* shld eax, edx, cl */
    uint8_t shrd_imm[] = {0x0f, 0xac, 0xd0, 0x04}; /* shrd eax, edx, 4 */
    uint8_t shrd_cl[] = {0x0f, 0xad, 0xd0};        /* shrd eax, edx, cl */
    uint8_t shld_r16[] = {0x66, 0x0f, 0xa4, 0xd0, 0x04}; /* shld ax, dx, 4 */

    ASSERT(hb_decode_x86(shld_imm, sizeof(shld_imm), 0x7ac28473, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHLD && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 4);
    ASSERT(d.op3.is_imm && d.op3.imm == 4);

    ASSERT(hb_decode_x86(shld_cl, sizeof(shld_cl), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHLD && d.len == 3);
    ASSERT(d.op3.is_reg && d.op3.reg == HB_REG_RCX && d.op3.size == 1);

    ASSERT(hb_decode_x86(shrd_imm, sizeof(shrd_imm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHRD && d.len == 4);
    ASSERT(d.op3.is_imm && d.op3.imm == 4);

    ASSERT(hb_decode_x86(shrd_cl, sizeof(shrd_cl), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHRD && d.len == 3);
    ASSERT(d.op3.is_reg && d.op3.reg == HB_REG_RCX && d.op3.size == 1);

    ASSERT(hb_decode_x86(shld_r16, sizeof(shld_r16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHLD && d.op1.size == 2 && d.op2.size == 2);

    uint8_t shld_r64[] = {0x48, 0x0f, 0xa4, 0xd0, 0x04}; /* shld rax, rdx, 4 */
    ASSERT(hb_decode_x64(shld_r64, sizeof(shld_r64), 0x140001000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHLD && d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 8);

    tests_passed++;
}

TEST(interp_x86_alu_modrm_operand16_preserves_upper) {
    const uint32_t code_base = 0x00406c00u;
    const uint32_t data_base = 0x0000c000u;
    uint8_t code[] = {
        0x66, 0x2b, 0x53, 0xfc, /* sub dx, word ptr [ebx-4] */
        0x66, 0x03, 0x48, 0x02  /* add cx, word ptr [eax+2] */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u16(ctx->memory, data_base, 0x0010) == HB_OK);
    ASSERT(hb_memory_write_u16(ctx->memory, data_base + 2, 0x0003) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ebx = data_base + 4;
    ctx->regs.x86.ecx = 0xaaaa0007u;
    ctx->regs.x86.edx = 0x12340030u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.edx == 0x12340020u);
    ASSERT(ctx->regs.x86.ecx == 0xaaaa000au);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_lock_alu_rmw_family) {
    const uint32_t code_base = 0x00406d00u;
    const uint32_t data_base = 0x0000e000u;
    uint8_t code[] = {
        0xf0, 0x21, 0x08,       /* lock and dword ptr [eax], ecx */
        0xf0, 0x83, 0x08, 0x7f, /* lock or dword ptr [eax], 0x7f */
        0xf0, 0xf7, 0x18        /* lock neg dword ptr [eax] */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base, 0xff00ff00u) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ecx = 0x0f0f0f0fu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t value = 0;
    ASSERT(hb_memory_read_u32(ctx->memory, data_base, &value) == HB_OK);
    ASSERT(value == 0xf0fff081u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_byte_shift_rotate_family) {
    const uint32_t code_base = 0x0040a100u;
    const uint32_t data_base = 0x0000d000u;
    uint8_t code[] = {
        0xc0, 0xe2, 0x04, /* shl dl, 4 */
        0xd0, 0x6e, 0x03, /* shr byte ptr [esi+3], 1 */
        0xd2, 0xcb        /* ror bl, cl */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u8(ctx->memory, data_base + 3, 0x80) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = 0x12345612u;
    ctx->regs.x86.ecx = 0x00000004u;
    ctx->regs.x86.edx = 0x8765430au;
    ctx->regs.x86.esi = data_base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.edx == 0x876543a0u);
    ASSERT(ctx->regs.x86.ebx == 0x12345621u);
    uint8_t got = 0;
    ASSERT(hb_memory_read_u8(ctx->memory, data_base + 3, &got) == HB_OK);
    ASSERT(got == 0x40);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_double_shift_family) {
    const uint32_t code_base = 0x0040a180u;
    uint8_t code[] = {
        0x0f, 0xa4, 0xd0, 0x04, /* shld eax, edx, 4 */
        0x0f, 0xac, 0xd8, 0x04  /* shrd eax, ebx, 4 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x12345678u;
    ctx->regs.x86.ebx = 0xf0000001u;
    ctx->regs.x86.edx = 0x9abcdef0u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x12345678u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_xchg_leave_family) {
    hb_decoded_t d;
    uint8_t xchg_rm32_reg32[] = {0x87, 0x23};       /* xchg dword ptr [ebx], esp */
    uint8_t xchg_reg8_high8[] = {0x86, 0xe0};       /* xchg al, ah */
    uint8_t xchg_eax_ebx[] = {0x93};                /* xchg eax, ebx */
    uint8_t xchg_ax_di[] = {0x66, 0x97};            /* xchg ax, di */
    uint8_t leave32[] = {0xc9};                     /* leave */
    uint8_t leave16[] = {0x66, 0xc9};               /* leavew */

    ASSERT(hb_decode_x86(xchg_rm32_reg32, sizeof(xchg_rm32_reg32), 0x7bdb0534u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG && d.len == 2);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RSP && d.op2.size == 4);
    ASSERT(!d.writes_flags);

    ASSERT(hb_decode_x86(xchg_reg8_high8, sizeof(xchg_reg8_high8), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG && d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 1 && d.op1.reg_offset == 0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 1 && d.op2.reg_offset == 1);

    ASSERT(hb_decode_x86(xchg_eax_ebx, sizeof(xchg_eax_ebx), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG && d.len == 1);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RBX && d.op2.size == 4);

    ASSERT(hb_decode_x86(xchg_ax_di, sizeof(xchg_ax_di), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XCHG && d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDI && d.op2.size == 2);

    ASSERT(hb_decode_x86(leave32, sizeof(leave32), 0x7bdb064fu, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LEAVE && d.len == 1);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBP && d.op1.size == 4);
    ASSERT(d.stack_delta == 4);

    ASSERT(hb_decode_x86(leave16, sizeof(leave16), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LEAVE && d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBP && d.op1.size == 2);
    ASSERT(d.stack_delta == 2);

    tests_passed++;
}

TEST(decode_x86_segment_mov_family) {
    hb_decoded_t d;
    uint8_t mov_mem_gs[] = {0x8c, 0xa8, 0x8c, 0x00, 0x00, 0x00}; /* mov [eax+0x8c], gs */
    uint8_t mov_mem_fs[] = {0x8c, 0xa0, 0x90, 0x00, 0x00, 0x00}; /* mov [eax+0x90], fs */
    uint8_t mov_fs_mem[] = {0x8e, 0x66, 0x04};                   /* mov fs, [esi+4] */
    uint8_t mov_ax_ds[] = {0x8c, 0xd8};                           /* mov ax, ds */

    ASSERT(hb_decode_x86(mov_mem_gs, sizeof(mov_mem_gs), 0x7bdb04c7u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG && d.len == 6);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX && d.op1.mem.disp == 0x8c && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 5 && d.op2.size == 2);
    ASSERT(!d.writes_flags);

    ASSERT(hb_decode_x86(mov_mem_fs, sizeof(mov_mem_fs), 0x7bdb04cdu, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG && d.len == 6);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RAX && d.op1.mem.disp == 0x90 && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 4 && d.op2.size == 2);

    ASSERT(hb_decode_x86(mov_fs_mem, sizeof(mov_fs_mem), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG && d.len == 3);
    ASSERT(d.op1.is_imm && d.op1.imm == 4 && d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RSI && d.op2.mem.disp == 4 && d.op2.size == 2);

    ASSERT(hb_decode_x86(mov_ax_ds, sizeof(mov_ax_ds), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV_SEG && d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 3 && d.op2.size == 2);

    tests_passed++;
}

TEST(interp_x86_xchg_leave_family) {
    const uint32_t code_base = 0x0040a300u;
    const uint32_t data_base = 0x0000e000u;
    const uint32_t stack_base = 0x00010000u;
    uint8_t code[] = {
        0x93,             /* xchg eax, ebx */
        0x87, 0x06,       /* xchg dword ptr [esi], eax */
        0xc9              /* leave */
    };
    uint32_t got = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base, 0x33333333u) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, stack_base + 0x80, 0x44444444u) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x11111111u;
    ctx->regs.x86.ebx = 0x22222222u;
    ctx->regs.x86.esi = data_base;
    ctx->regs.x86.ebp = stack_base + 0x80;
    ctx->regs.x86.esp = stack_base + 0x40;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x33333333u);
    ASSERT(ctx->regs.x86.ebx == 0x11111111u);
    ASSERT(ctx->regs.x86.ebp == 0x44444444u);
    ASSERT(ctx->regs.x86.esp == stack_base + 0x84);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base, &got) == HB_OK);
    ASSERT(got == 0x22222222u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_segment_mov_family) {
    const uint32_t code_base = 0x0040a320u;
    const uint32_t data_base = 0x0000f000u;
    uint8_t code[] = {
        0x8c, 0x06,       /* mov [esi], es */
        0x8c, 0x6e, 0x02, /* mov [esi+2], gs */
        0x8e, 0x66, 0x04, /* mov fs, [esi+4] */
        0x8c, 0xe0        /* mov ax, fs */
    };
    uint16_t got16 = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u16(ctx->memory, data_base + 4, 0x7777u) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x12340000u;
    ctx->regs.x86.esi = data_base;
    ctx->seg_es = 0x0023u;
    ctx->seg_fs = 0x0053u;
    ctx->seg_gs = 0x002bu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u16(ctx->memory, data_base, &got16) == HB_OK);
    ASSERT(got16 == 0x0023u);
    ASSERT(hb_memory_read_u16(ctx->memory, data_base + 2, &got16) == HB_OK);
    ASSERT(got16 == 0x002bu);
    ASSERT(ctx->seg_fs == 0x7777u);
    ASSERT((ctx->regs.x86.eax & 0xffffu) == 0x7777u);
    ASSERT((ctx->regs.x86.eax & 0xffff0000u) == 0x12340000u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_pushf_popf_family) {
    hb_decoded_t d;
    uint8_t pushfd[] = {0x9c};
    uint8_t popfd[] = {0x9d};
    uint8_t pushfw[] = {0x66, 0x9c};
    uint8_t popfw[] = {0x66, 0x9d};

    ASSERT(hb_decode_x86(pushfd, sizeof(pushfd), 0x7bdb0515u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSHF && d.len == 1);
    ASSERT(d.op1.is_imm && d.op1.size == 4);
    ASSERT(d.stack_delta == 4);

    ASSERT(hb_decode_x86(popfd, sizeof(popfd), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POPF && d.len == 1);
    ASSERT(d.op1.is_imm && d.op1.size == 4);
    ASSERT(d.stack_delta == 4);

    ASSERT(hb_decode_x86(pushfw, sizeof(pushfw), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUSHF && d.len == 2);
    ASSERT(d.op1.is_imm && d.op1.size == 2);
    ASSERT(d.stack_delta == 2);

    ASSERT(hb_decode_x86(popfw, sizeof(popfw), 0x1000u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_POPF && d.len == 2);
    ASSERT(d.op1.is_imm && d.op1.size == 2);
    ASSERT(d.stack_delta == 2);

    tests_passed++;
}

TEST(interp_x86_pushf_popf_family) {
    const uint32_t code_base = 0x0040a340u;
    const uint32_t data_base = 0x00011000u;
    const uint32_t stack_base = 0x00012000u;
    uint8_t code[] = {
        0x9c,                         /* pushfd */
        0x8f, 0x06,                   /* pop dword [esi] */
        0x66, 0x9c,                   /* pushfw */
        0x66, 0x9d,                   /* popfw */
        0x9d                          /* popfd */
    };
    uint32_t got32 = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, stack_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, stack_base + 0x80, 0x00000495u) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esi = data_base;
    ctx->regs.x86.esp = stack_base + 0x80;
    ctx->regs.x86.eflags = 0x00000202u;
    ctx->flags.cf = true;
    ctx->flags.pf = true;
    ctx->flags.zf = true;
    ctx->flags.of = true;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base, &got32) == HB_OK);
    ASSERT(got32 == 0x00000a47u);
    ASSERT(ctx->regs.x86.esp == stack_base + 0x84);
    ASSERT((ctx->regs.x86.eflags & 0xffffu) == 0x0497u);
    ASSERT(ctx->flags.cf && ctx->flags.pf && ctx->flags.af && ctx->flags.sf);
    ASSERT(!ctx->flags.zf && !ctx->flags.of);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_imul_family) {
    hb_decoded_t d;
    uint8_t imul_imm32[] = {0x69, 0xd2, 0x01, 0x01, 0x01, 0x01}; /* imul edx, edx, 0x01010101 */
    uint8_t imul_imm8[] = {0x6b, 0xc1, 0xfb};                   /* imul eax, ecx, -5 */
    uint8_t imul_rm[] = {0x0f, 0xaf, 0xd8};                     /* imul ebx, eax */
    uint8_t imul_r16_imm8[] = {0x66, 0x6b, 0xd2, 0x03};         /* imul dx, dx, 3 */

    ASSERT(hb_decode_x86(imul_imm32, sizeof(imul_imm32), 0x7bdb1760u, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IMUL && d.len == 6);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 4);
    ASSERT(d.op3.is_imm && d.op3.imm == 0x01010101 && d.op3.size == 4);

    ASSERT(hb_decode_x86(imul_imm8, sizeof(imul_imm8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IMUL && d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.size == 4);
    ASSERT(d.op3.is_imm && d.op3.imm == -5 && d.op3.size == 4);

    ASSERT(hb_decode_x86(imul_rm, sizeof(imul_rm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IMUL && d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RBX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 4);

    ASSERT(hb_decode_x86(imul_r16_imm8, sizeof(imul_r16_imm8), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_IMUL && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 2);
    ASSERT(d.op3.is_imm && d.op3.imm == 3 && d.op3.size == 2);

    tests_passed++;
}

TEST(interp_x86_imul_family) {
    const uint32_t code_base = 0x00406d00u;
    uint8_t code[] = {
        0x69, 0xd2, 0x01, 0x01, 0x01, 0x01, /* imul edx, edx, 0x01010101 */
        0x6b, 0xc1, 0xfb,                   /* imul eax, ecx, -5 */
        0x0f, 0xaf, 0xd8,                   /* imul ebx, eax */
        0x66, 0x6b, 0xd2, 0x03              /* imul dx, dx, 3 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0;
    ctx->regs.x86.ebx = 2;
    ctx->regs.x86.ecx = 7;
    ctx->regs.x86.edx = 3;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0xffffffddu);
    ASSERT(ctx->regs.x86.ebx == 0xffffffbau);
    ASSERT(ctx->regs.x86.ecx == 7);
    ASSERT(ctx->regs.x86.edx == 0x03030909u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_gpr_transfer_family) {
    hb_decoded_t d;
    uint8_t movd_xmm_r32[] = {0x66, 0x0f, 0x6e, 0xc2}; /* movd xmm0, edx */
    uint8_t movd_r32_xmm[] = {0x66, 0x0f, 0x7e, 0xc0}; /* movd eax, xmm0 */
    uint8_t movq_mem_xmm[] = {0x66, 0x0f, 0xd6, 0x0b}; /* movq [ebx], xmm1 */
    uint8_t movq_xmm_xmm[] = {0xf3, 0x0f, 0x7e, 0xc8}; /* movq xmm1, xmm0 */

    ASSERT(hb_decode_x86(movd_xmm_r32, sizeof(movd_xmm_r32), 0x7bdb177b, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.size == 4);

    ASSERT(hb_decode_x86(movd_r32_xmm, sizeof(movd_r32_xmm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movq_mem_xmm, sizeof(movq_mem_xmm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD && d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movq_xmm_xmm, sizeof(movq_xmm_xmm), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 16);

    tests_passed++;
}

TEST(interp_x86_sse_gpr_transfer_family) {
    const uint32_t code_base = 0x00406e00u;
    const uint32_t data_base = 0x0000d000u;
    uint8_t code[] = {
        0x66, 0x0f, 0x6e, 0xc2, /* movd xmm0, edx */
        0x66, 0x0f, 0x7e, 0xc0, /* movd eax, xmm0 */
        0xf3, 0x0f, 0x7e, 0xc8, /* movq xmm1, xmm0 */
        0x66, 0x0f, 0xd6, 0x0b  /* movq [ebx], xmm1 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = data_base;
    ctx->regs.x86.edx = 0x11223344u;
    ctx->regs.x86.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x86.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x11223344u);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0x11223344ULL);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0);
    ASSERT(ctx->regs.x86.xmm[1][0] == 0x11223344ULL);
    ASSERT(ctx->regs.x86.xmm[1][1] == 0);
    uint64_t stored = 0;
    ASSERT(hb_memory_read_u64(ctx->memory, data_base, &stored) == HB_OK);
    ASSERT(stored == 0x11223344ULL);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_prefixed_packed_mov_family) {
    hb_decoded_t d;
    uint8_t movdqu_store[] = {0xf3, 0x0f, 0x7f, 0x03}; /* movdqu [ebx], xmm0 */
    uint8_t movdqu_load[] = {0xf3, 0x0f, 0x6f, 0xc1};  /* movdqu xmm0, xmm1 */
    uint8_t movsd_load[] = {0xf2, 0x0f, 0x10, 0xc1};   /* movsd xmm0, xmm1 */

    ASSERT(hb_decode_x86(movdqu_store, sizeof(movdqu_store), 0x7bdb1788, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV && d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movdqu_load, sizeof(movdqu_load), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    ASSERT(hb_decode_x86(movsd_load, sizeof(movsd_load), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 8);

    tests_passed++;
}

TEST(interp_x86_sse_prefixed_packed_mov_family) {
    const uint32_t code_base = 0x00407100u;
    const uint32_t data_base = 0x0000e000u;
    uint8_t code[] = {
        0xf3, 0x0f, 0x7f, 0x03, /* movdqu [ebx], xmm0 */
        0xf3, 0x0f, 0x6f, 0x0b  /* movdqu xmm1, [ebx] */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.ebx = data_base;
    ctx->regs.x86.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x86.xmm[0][1] = 0x99aabbccddeeff00ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint64_t lane0 = 0, lane1 = 0;
    ASSERT(hb_memory_read_u64(ctx->memory, data_base, &lane0) == HB_OK);
    ASSERT(hb_memory_read_u64(ctx->memory, data_base + 8, &lane1) == HB_OK);
    ASSERT(lane0 == 0x1122334455667788ULL);
    ASSERT(lane1 == 0x99aabbccddeeff00ULL);
    ASSERT(ctx->regs.x86.xmm[1][0] == 0x1122334455667788ULL);
    ASSERT(ctx->regs.x86.xmm[1][1] == 0x99aabbccddeeff00ULL);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_unpack_family) {
    hb_decoded_t d;
    struct {
        uint8_t op2;
        int ins;
    } punpck_cases[] = {
        {0x60, HB_INS_PUNPCKLBW}, {0x61, HB_INS_PUNPCKLWD},
        {0x62, HB_INS_PUNPCKLDQ}, {0x6c, HB_INS_PUNPCKLQDQ},
        {0x68, HB_INS_PUNPCKHBW}, {0x69, HB_INS_PUNPCKHWD},
        {0x6a, HB_INS_PUNPCKHDQ}, {0x6d, HB_INS_PUNPCKHQDQ},
    };

    for (unsigned i = 0; i < sizeof(punpck_cases) / sizeof(punpck_cases[0]); i++) {
        uint8_t code[] = {0x66, 0x0f, punpck_cases[i].op2, 0xc1};
        ASSERT(hb_decode_x86(code, sizeof(code), 0x7bdb177f, &d) == HB_OK);
        ASSERT((int)d.opcode == punpck_cases[i].ins && d.len == 4);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);
    }

    uint8_t unpcklps[] = {0x0f, 0x14, 0xc1};
    uint8_t unpcklpd[] = {0x66, 0x0f, 0x14, 0xc1};
    uint8_t unpckhps[] = {0x0f, 0x15, 0xc1};
    uint8_t unpckhpd[] = {0x66, 0x0f, 0x15, 0xc1};

    ASSERT(hb_decode_x86(unpcklps, sizeof(unpcklps), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UNPCKLPS && d.op1.reg == HB_REG_XMM0 && d.op2.reg == HB_REG_XMM1);
    ASSERT(hb_decode_x86(unpcklpd, sizeof(unpcklpd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UNPCKLPD && d.op1.reg == HB_REG_XMM0 && d.op2.reg == HB_REG_XMM1);
    ASSERT(hb_decode_x86(unpckhps, sizeof(unpckhps), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UNPCKHPS && d.op1.reg == HB_REG_XMM0 && d.op2.reg == HB_REG_XMM1);
    ASSERT(hb_decode_x86(unpckhpd, sizeof(unpckhpd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_UNPCKHPD && d.op1.reg == HB_REG_XMM0 && d.op2.reg == HB_REG_XMM1);

    tests_passed++;
}

TEST(interp_x86_sse_unpack_family) {
    const uint32_t code_base = 0x00406f00u;
    uint8_t code[] = {
        0x66, 0x0f, 0x62, 0xc1, /* punpckldq xmm0, xmm1 */
        0x66, 0x0f, 0x69, 0xd3  /* punpckhwd xmm2, xmm3 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.xmm[0][0] = 0x2222222211111111ULL;
    ctx->regs.x86.xmm[0][1] = 0x4444444433333333ULL;
    ctx->regs.x86.xmm[1][0] = 0xbbbbbbbbaaaaaaaaULL;
    ctx->regs.x86.xmm[1][1] = 0xddddddddccccccccULL;
    ctx->regs.x86.xmm[2][0] = 0x1003100210011000ULL;
    ctx->regs.x86.xmm[2][1] = 0x1007100610051004ULL;
    ctx->regs.x86.xmm[3][0] = 0x2003200220012000ULL;
    ctx->regs.x86.xmm[3][1] = 0x2007200620052004ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0xaaaaaaaa11111111ULL);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0xbbbbbbbb22222222ULL);
    ASSERT(ctx->regs.x86.xmm[2][0] == 0x2005100520041004ULL);
    ASSERT(ctx->regs.x86.xmm[2][1] == 0x2007100720061006ULL);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_shuffle_family) {
    hb_decoded_t d;
    struct {
        uint8_t prefix;
        int ins;
    } pshuf_cases[] = {
        {0x66, HB_INS_PSHUFD},
        {0xf2, HB_INS_PSHUFLW},
        {0xf3, HB_INS_PSHUFHW},
    };

    for (unsigned i = 0; i < sizeof(pshuf_cases) / sizeof(pshuf_cases[0]); i++) {
        uint8_t code[] = {pshuf_cases[i].prefix, 0x0f, 0x70, 0xc1, 0x1b};
        ASSERT(hb_decode_x86(code, sizeof(code), 0x7bdb1783, &d) == HB_OK);
        ASSERT((int)d.opcode == pshuf_cases[i].ins && d.len == 5);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);
        ASSERT(d.op3.is_imm && d.op3.imm == 0x1b);
    }

    uint8_t pshufb[] = {0x66, 0x0f, 0x38, 0x00, 0xc1};
    ASSERT(hb_decode_x86(pshufb, sizeof(pshufb), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSHUFB && d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    tests_passed++;
}

TEST(interp_x86_sse_shuffle_family) {
    const uint32_t code_base = 0x00407000u;
    uint8_t code[] = {
        0x66, 0x0f, 0x70, 0xc1, 0x1b, /* pshufd xmm0, xmm1, 0x1b */
        0xf2, 0x0f, 0x70, 0xd3, 0x1b  /* pshuflw xmm2, xmm3, 0x1b */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.xmm[1][0] = 0x0000000200000001ULL;
    ctx->regs.x86.xmm[1][1] = 0x0000000400000003ULL;
    ctx->regs.x86.xmm[3][0] = 0x1003100210011000ULL;
    ctx->regs.x86.xmm[3][1] = 0x1007100610051004ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0x0000000300000004ULL);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0x0000000100000002ULL);
    ASSERT(ctx->regs.x86.xmm[2][0] == 0x1000100110021003ULL);
    ASSERT(ctx->regs.x86.xmm[2][1] == 0x1007100610051004ULL);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_word_insert_extract_family) {
    hb_decoded_t d;
    uint8_t pinsrw_mem[] = {0x66, 0x0f, 0xc4, 0x40, 0x02, 0x01};
    ASSERT(hb_decode_x86(pinsrw_mem, sizeof(pinsrw_mem), 0x7bd7e09b, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PINSRW && d.len == 6);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_mem && d.op2.size == 2 && d.op2.mem.base == HB_REG_RAX && d.op2.mem.disp == 2);
    ASSERT(d.op3.is_imm && d.op3.imm == 1);

    uint8_t pextrw_reg[] = {0x66, 0x0f, 0xc5, 0xc1, 0x03};
    ASSERT(hb_decode_x86(pextrw_reg, sizeof(pextrw_reg), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PEXTRW && d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);
    ASSERT(d.op3.is_imm && d.op3.imm == 3);

    tests_passed++;
}

TEST(interp_x86_sse_word_insert_extract_family) {
    const uint32_t code_base = 0x00408000u;
    const uint32_t data_base = 0x00500000u;
    uint8_t code[] = {
        0x66, 0x0f, 0xc4, 0x40, 0x02, 0x01, /* pinsrw xmm0, word [eax+2], 1 */
        0x66, 0x0f, 0xc5, 0xc8, 0x01        /* pextrw ecx, xmm0, 1 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map_private(ctx->memory, data_base, 0x1000, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u16(ctx->memory, data_base + 2, 0xabcd) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ecx = 0xffffffffu;
    ctx->regs.x86.xmm[0][0] = 0x0000000000001111ULL;
    ctx->regs.x86.xmm[0][1] = 0x2222333344445555ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0x00000000abcd1111ULL);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0x2222333344445555ULL);
    ASSERT(ctx->regs.x86.ecx == 0x0000abcdu);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_packed_integer_add_sub_family) {
    hb_decoded_t d;
    struct {
        uint8_t op2;
        int ins;
    } cases[] = {
        {0xfc, HB_INS_PADDB},
        {0xfd, HB_INS_PADDW},
        {0xfe, HB_INS_PADDD},
        {0xd4, HB_INS_PADDQ},
        {0xf8, HB_INS_PSUBB},
        {0xf9, HB_INS_PSUBW},
        {0xfa, HB_INS_PSUBD},
        {0xfb, HB_INS_PSUBQ},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code[] = {0x66, 0x0f, cases[i].op2, 0xc1};
        ASSERT(hb_decode_x86(code, sizeof(code), 0x7bd7e0f1, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].ins && d.len == 4);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);
    }

    tests_passed++;
}

TEST(interp_x86_sse_packed_integer_add_sub_family) {
    const uint32_t code_base = 0x00409000u;
    uint8_t code[] = {
        0x66, 0x0f, 0xfe, 0xc1, /* paddd xmm0, xmm1 */
        0x66, 0x0f, 0xf9, 0xd3  /* psubw xmm2, xmm3 */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.xmm[0][0] = 0x0000000200000001ULL;
    ctx->regs.x86.xmm[0][1] = 0x0000000400000003ULL;
    ctx->regs.x86.xmm[1][0] = 0x000000140000000aULL;
    ctx->regs.x86.xmm[1][1] = 0x000000280000001eULL;

    uint16_t lhs[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    uint16_t rhs[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memcpy(ctx->regs.x86.xmm[2], lhs, sizeof(lhs));
    memcpy(ctx->regs.x86.xmm[3], rhs, sizeof(rhs));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.xmm[0][0] == 0x000000160000000bULL);
    ASSERT(ctx->regs.x86.xmm[0][1] == 0x0000002c00000021ULL);
    uint16_t got[8];
    memcpy(got, ctx->regs.x86.xmm[2], sizeof(got));
    for (unsigned i = 0; i < 8; i++) ASSERT(got[i] == (uint16_t)(lhs[i] - rhs[i]));
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_sse_compare_mask_family) {
    hb_decoded_t d;
    struct {
        uint8_t op2;
        int ins;
    } cmp_cases[] = {
        {0x64, HB_INS_PCMPGTB},
        {0x65, HB_INS_PCMPGTW},
        {0x66, HB_INS_PCMPGTD},
        {0x74, HB_INS_PCMPEQB},
        {0x75, HB_INS_PCMPEQW},
        {0x76, HB_INS_PCMPEQD},
    };

    for (unsigned i = 0; i < sizeof(cmp_cases) / sizeof(cmp_cases[0]); i++) {
        uint8_t code[] = {0x66, 0x0f, cmp_cases[i].op2, 0xc1};
        ASSERT(hb_decode_x86(code, sizeof(code), 0x7bd8eb06, &d) == HB_OK);
        ASSERT((int)d.opcode == cmp_cases[i].ins && d.len == 4);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);
    }

    uint8_t pmovmskb[] = {0x66, 0x0f, 0xd7, 0xf1}; /* pmovmskb esi, xmm1 */
    ASSERT(hb_decode_x86(pmovmskb, sizeof(pmovmskb), 0x7bd8eb0e, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PMOVMSKB && d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RSI && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    tests_passed++;
}

TEST(interp_x86_pcmpeqb_pmovmskb_notepadpp_vector_compare) {
    const uint32_t code_base = 0x00409500u;
    const uint32_t data_base = 0x00510000u;
    uint8_t code[] = {
        0xf3, 0x0f, 0x6f, 0x81, 0xa0, 0x00, 0x00, 0x00, /* movdqu xmm0, [ecx+0xa0] */
        0xf3, 0x0f, 0x6f, 0x08,                         /* movdqu xmm1, [eax] */
        0x66, 0x0f, 0x74, 0xc8,                         /* pcmpeqb xmm1, xmm0 */
        0x66, 0x0f, 0xd7, 0xf1                          /* pmovmskb esi, xmm1 */
    };
    uint8_t rhs[16] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00
    };
    uint8_t lhs[16] = {
        0x10, 0x21, 0x30, 0x41, 0x50, 0x61, 0x70, 0x81,
        0x90, 0xa1, 0xb0, 0xc1, 0xd0, 0xe1, 0xf0, 0x01
    };

    hb_decoded_t d;
    ASSERT(hb_decode_x86(code + 12, 4, code_base + 12, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPEQB);
    ASSERT(hb_decode_x86(code + 16, 4, code_base + 16, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PMOVMSKB);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 0x1000,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base, lhs, sizeof(lhs)) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, data_base + 0xa0, rhs, sizeof(rhs)) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ecx = data_base;
    ctx->regs.x86.esi = 0xffffffffu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.esi == 0x5555u);
    ASSERT(ctx->regs.x86.xmm[1][0] == 0x00ff00ff00ff00ffULL);
    ASSERT(ctx->regs.x86.xmm[1][1] == 0x00ff00ff00ff00ffULL);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_bswap_family) {
    const uint32_t code_base = 0x0040a000u;
    uint8_t code[] = {
        0x0f, 0xc8, /* bswap eax */
        0x0f, 0xcb  /* bswap ebx */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0x12345678u;
    ctx->regs.x86.ebx = 0xa1b2c3d4u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x78563412u);
    ASSERT(ctx->regs.x86.ebx == 0xd4c3b2a1u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_lock_atomic_family) {
    hb_decoded_t d;
    uint8_t lock_inc[] = {0xf0, 0xff, 0x46, 0x04};       /* lock inc dword [esi+4] */
    uint8_t lock_cmpxchg[] = {0xf0, 0x0f, 0xb1, 0x56, 0x08}; /* lock cmpxchg edx,[esi+8] */
    uint8_t lock_xadd[] = {0xf0, 0x0f, 0xc1, 0x0b};      /* lock xadd ecx,[ebx] */
    uint8_t lock_cmpxchg8b[] = {0xf0, 0x0f, 0xc7, 0x0f}; /* lock cmpxchg8b [edi] */

    ASSERT(hb_decode_x86(lock_inc, sizeof(lock_inc), 0x7bdb518f, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_INC);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RSI);
    ASSERT(d.op1.mem.disp == 4);
    ASSERT(d.op1.size == 4);

    ASSERT(hb_decode_x86(lock_cmpxchg, sizeof(lock_cmpxchg), 0x7bdb5186, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMPXCHG);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RSI);
    ASSERT(d.op1.mem.disp == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX);

    ASSERT(hb_decode_x86(lock_xadd, sizeof(lock_xadd), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XADD);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RBX);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX);

    ASSERT(hb_decode_x86(lock_cmpxchg8b, sizeof(lock_cmpxchg8b), 0x7bc56d1f, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CMPXCHG8B);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_mem && d.op1.mem.base == HB_REG_RDI);
    ASSERT(d.op1.size == 8);
    tests_passed++;
}

TEST(interp_x86_lock_inc_cmpxchg_critical_section) {
    const uint32_t code_base = 0x00407000u;
    const uint32_t data_base = 0x00008000u;
    uint8_t code[] = {
        0xf0, 0xff, 0x46, 0x04,       /* lock inc dword [esi+4] */
        0xf0, 0x0f, 0xb1, 0x56, 0x08  /* lock cmpxchg edx, [esi+8] */
    };
    uint32_t value = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base + 4, 0xffffffffu) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base + 8, 0xffffffffu) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esi = data_base;
    ctx->regs.x86.eax = 0xffffffffu;
    ctx->regs.x86.edx = 0x12345678u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base + 4, &value) == HB_OK);
    ASSERT(value == 0);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base + 8, &value) == HB_OK);
    ASSERT(value == 0x12345678u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_loader_section_recursive_owner_jcc) {
    const uint32_t code_base = 0x00409000u;
    const uint32_t loader_section = 0x7be1b1d8u;
    const uint32_t teb_guest = 0x00200000u;
    uint8_t code[] = {
        0x8b, 0x46, 0x0c,                               /* mov eax, [esi+0xc] */
        0x64, 0x8b, 0x0d, 0x18, 0x00, 0x00, 0x00,       /* mov ecx, fs:[0x18] */
        0x3b, 0x41, 0x24,                               /* cmp eax, [ecx+0x24] */
        0x74, 0x02,                                     /* je recursive_owner */
        0x90,                                           /* fallthrough padding */
        0x90                                            /* recursive_owner target */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, code_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, loader_section & ~0xfffu, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_guest32_map(ctx->memory, teb_guest & ~0xfffu, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write(ctx->memory, code_base, code, sizeof(code)) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, loader_section + 12, 0x2c) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, teb_guest + 0x18, teb_guest) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, teb_guest + 0x24, 0x2c) == HB_OK);

    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esi = loader_section;
    ctx->fs_base = teb_guest;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x2cu);
    ASSERT(ctx->regs.x86.ecx == teb_guest);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_cmpxchg8b_slist_pair) {
    const uint32_t code_base = 0x00408000u;
    const uint32_t data_base = 0x00009000u;
    uint8_t code[] = {
        0xf0, 0x0f, 0xc7, 0x0f,       /* lock cmpxchg8b [edi] */
        0x0f, 0xc7, 0x4f, 0x08        /* cmpxchg8b [edi+8] */
    };
    uint64_t value = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u64(ctx->memory, data_base, 0x1122334455667788ULL) == HB_OK);
    ASSERT(hb_memory_write_u64(ctx->memory, data_base + 8, 0x0102030405060708ULL) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.edi = data_base;
    ctx->regs.x86.eax = 0x55667788u;
    ctx->regs.x86.edx = 0x11223344u;
    ctx->regs.x86.ebx = 0xeeff0011u;
    ctx->regs.x86.ecx = 0xaabbccddu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u64(ctx->memory, data_base, &value) == HB_OK);
    ASSERT(value == 0xaabbccddeeff0011ULL);
    ASSERT(hb_memory_read_u64(ctx->memory, data_base + 8, &value) == HB_OK);
    ASSERT(value == 0x0102030405060708ULL);
    ASSERT(ctx->regs.x86.eax == 0x05060708u);
    ASSERT(ctx->regs.x86.edx == 0x01020304u);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_movzx_movsx_family) {
    hb_decoded_t d;
    uint8_t movzbl_abs[] = {0x0f, 0xb6, 0x05, 0xcc, 0xc0, 0xc8, 0x7b};
    uint8_t movzwl_mem[] = {0x0f, 0xb7, 0x4b, 0x02}; /* movzwl 2(ebx), ecx */
    uint8_t movsbl_mem[] = {0x0f, 0xbe, 0x10};       /* movsbl (eax), edx */
    uint8_t movswl_reg[] = {0x0f, 0xbf, 0xf8};       /* movswl ax, edi */

    ASSERT(hb_decode_x86(movzbl_abs, sizeof(movzbl_abs), 0x7bd7ecf2, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVZX);
    ASSERT(d.len == 7);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1);
    ASSERT(d.op2.mem.disp == 0x7bc8c0cc);
    ASSERT(d.op2.size == 1);

    ASSERT(hb_decode_x86(movzwl_mem, sizeof(movzwl_mem), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVZX);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RBX);
    ASSERT(d.op2.mem.disp == 2);
    ASSERT(d.op2.size == 2);

    ASSERT(hb_decode_x86(movsbl_mem, sizeof(movsbl_mem), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVSX);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RAX);
    ASSERT(d.op2.size == 1);

    ASSERT(hb_decode_x86(movswl_reg, sizeof(movswl_reg), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVSX);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDI);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX);
    ASSERT(d.op2.size == 2);
    tests_passed++;
}

TEST(interp_x86_movzx_movsx_memory_family) {
    const uint32_t code_base = 0x00408000u;
    const uint32_t data_base = 0x00009000u;
    uint8_t code[] = {
        0x0f, 0xb6, 0x05, 0x00, 0x90, 0x00, 0x00, /* movzbl [0x9000], eax */
        0x0f, 0xbe, 0x0d, 0x01, 0x90, 0x00, 0x00, /* movsbl [0x9001], ecx */
        0x0f, 0xb7, 0x15, 0x02, 0x90, 0x00, 0x00  /* movzwl [0x9002], edx */
    };

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u8(ctx->memory, data_base, 0xfe) == HB_OK);
    ASSERT(hb_memory_write_u8(ctx->memory, data_base + 1, 0x80) == HB_OK);
    ASSERT(hb_memory_write_u16(ctx->memory, data_base + 2, 0xabcd) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0xfeu);
    ASSERT(ctx->regs.x86.ecx == 0xffffff80u);
    ASSERT(ctx->regs.x86.edx == 0xabcdu);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_legacy_high8_register_family_frame) {
    hb_decoded_t d;
    uint8_t setbe_ch[] = {0x0f, 0x96, 0xc5}; /* setbe ch */
    uint8_t mov_dh[] = {0xb6, 0x34};         /* mov dh, 0x34 */
    uint8_t test_dh[] = {0x84, 0xf6};        /* test dh, dh */
    uint8_t movzx_ch[] = {0x0f, 0xb6, 0xc5}; /* movzx eax, ch */
    uint8_t movsx_bh[] = {0x0f, 0xbe, 0xff}; /* movsx edi, bh */
    const uint32_t code_base = 0x0040b000u;
    uint8_t code[] = {
        0x39, 0xc0,       /* cmp eax, eax */
        0x0f, 0x96, 0xc5, /* setbe ch */
        0xb6, 0x34,       /* mov dh, 0x34 */
        0x84, 0xf6        /* test dh, dh */
    };

    ASSERT(hb_decode_x86(setbe_ch, sizeof(setbe_ch), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SETcc);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.reg_offset == 1);
    ASSERT(d.op1.size == 1);

    ASSERT(hb_decode_x86(mov_dh, sizeof(mov_dh), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.reg_offset == 1);
    ASSERT(d.op1.size == 1);

    ASSERT(hb_decode_x86(test_dh, sizeof(test_dh), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.reg_offset == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.reg_offset == 1);

    ASSERT(hb_decode_x86(movzx_ch, sizeof(movzx_ch), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVZX);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX && d.op2.reg_offset == 1);
    ASSERT(d.op2.size == 1);

    ASSERT(hb_decode_x86(movsx_bh, sizeof(movsx_bh), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVSX);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDI);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RBX && d.op2.reg_offset == 1);
    ASSERT(d.op2.size == 1);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = 0;
    ctx->regs.x86.ecx = 0x11220033u;
    ctx->regs.x86.edx = 0x55660077u;
    ctx->regs.x86.ebp = 0x00d5fb7cu;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.ecx == 0x11220133u);
    ASSERT(ctx->regs.x86.edx == 0x55663477u);
    ASSERT(ctx->regs.x86.ebp == 0x00d5fb7cu);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.zf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x86_accumulator_moffs_family) {
    hb_decoded_t d;
    uint8_t mov_al_moffs[] = {0xa0, 0x00, 0x90, 0x00, 0x00};
    uint8_t mov_eax_moffs[] = {0xa1, 0x58, 0x6d, 0xca, 0x7b};
    uint8_t mov_moffs_al[] = {0xa2, 0x04, 0x90, 0x00, 0x00};
    uint8_t mov_moffs_eax[] = {0xa3, 0x08, 0x90, 0x00, 0x00};
    uint8_t mov_eax_fs[] = {0x64, 0xa1, 0x18, 0x00, 0x00, 0x00};

    ASSERT(hb_decode_x86(mov_al_moffs, sizeof(mov_al_moffs), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 1);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1 && d.op2.mem.disp == 0x9000);
    ASSERT(d.op2.size == 1);

    ASSERT(hb_decode_x86(mov_eax_moffs, sizeof(mov_eax_moffs), 0x7bdb66a9, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1 && d.op2.mem.disp == 0x7bca6d58);
    ASSERT(d.op2.size == 4);

    ASSERT(hb_decode_x86(mov_moffs_al, sizeof(mov_moffs_al), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0x9004);
    ASSERT(d.op1.size == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 1);

    ASSERT(hb_decode_x86(mov_moffs_eax, sizeof(mov_moffs_eax), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0x9008);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 4);

    ASSERT(hb_decode_x86(mov_eax_fs, sizeof(mov_eax_fs), 0x7bc566bd, &d) == HB_OK);
    ASSERT(d.op2.is_mem && d.op2.mem.segment == 0x64 && d.op2.mem.disp == 0x18);
    tests_passed++;
}

TEST(decode_x86_mov_operand16_family) {
    hb_decoded_t d;
    uint8_t mov_rm16_r16[] = {0x66, 0x89, 0x58, 0x08}; /* mov [eax+8], bx */
    uint8_t mov_r16_rm16[] = {0x66, 0x8b, 0x48, 0x08}; /* mov cx, [eax+8] */
    uint8_t mov_ax_imm16[] = {0x66, 0xb8, 0x34, 0x12}; /* mov ax, 0x1234 */
    uint8_t mov_ax_moffs[] = {0x66, 0xa1, 0x78, 0x56, 0x34, 0x12};
    uint8_t mov_moffs_ax[] = {0x66, 0xa3, 0x7c, 0x56, 0x34, 0x12};

    ASSERT(hb_decode_x86(mov_rm16_r16, sizeof(mov_rm16_r16), 0x7bd81113, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV && d.len == sizeof(mov_rm16_r16));
    ASSERT(d.op1.is_mem && d.op1.size == 2 && d.op1.mem.base == HB_REG_RAX && d.op1.mem.disp == 8);
    ASSERT(d.op2.is_reg && d.op2.size == 2 && d.op2.reg == HB_REG_RBX);

    ASSERT(hb_decode_x86(mov_r16_rm16, sizeof(mov_r16_rm16), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV && d.len == sizeof(mov_r16_rm16));
    ASSERT(d.op1.is_reg && d.op1.size == 2 && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op2.is_mem && d.op2.size == 2 && d.op2.mem.base == HB_REG_RAX && d.op2.mem.disp == 8);

    ASSERT(hb_decode_x86(mov_ax_imm16, sizeof(mov_ax_imm16), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.imm == 0x1234 && d.op2.size == 2);

    ASSERT(hb_decode_x86(mov_ax_moffs, sizeof(mov_ax_moffs), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 2);
    ASSERT(d.op2.is_mem && d.op2.mem.base == -1 && d.op2.mem.disp == 0x12345678 && d.op2.size == 2);

    ASSERT(hb_decode_x86(mov_moffs_ax, sizeof(mov_moffs_ax), 0x1000, &d) == HB_OK);
    ASSERT(d.op1.is_mem && d.op1.mem.base == -1 && d.op1.mem.disp == 0x1234567c && d.op1.size == 2);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 2);
    tests_passed++;
}

TEST(interp_x86_accumulator_moffs_load_store) {
    const uint32_t code_base = 0x00409000u;
    const uint32_t data_base = 0x0000a000u;
    uint8_t code[] = {
        0xa1, 0x00, 0xa0, 0x00, 0x00, /* mov eax, [0xa000] */
        0xa2, 0x04, 0xa0, 0x00, 0x00, /* mov [0xa004], al */
        0xa3, 0x08, 0xa0, 0x00, 0x00  /* mov [0xa008], eax */
    };
    uint32_t value = 0;
    uint8_t byte = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_write_u32(ctx->memory, data_base, 0x12345678u) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x86.eax == 0x12345678u);
    ASSERT(hb_memory_read_u8(ctx->memory, data_base + 4, &byte) == HB_OK);
    ASSERT(byte == 0x78);
    ASSERT(hb_memory_read_u32(ctx->memory, data_base + 8, &value) == HB_OK);
    ASSERT(value == 0x12345678u);
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x86_mov_operand16_mem_reg_roundtrip) {
    const uint32_t code_base = 0x0040a000u;
    const uint32_t data_base = 0x0000b000u;
    uint8_t code[] = {
        0x66, 0x89, 0x58, 0x08, /* mov word ptr [eax+8], bx */
        0x66, 0x8b, 0x48, 0x08, /* mov cx, word ptr [eax+8] */
        0x66, 0xb8, 0x54, 0x76  /* mov ax, 0x7654 */
    };
    uint16_t value16 = 0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x86(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_guest32_map(ctx->memory, data_base, 4096,
                                 HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.eax = data_base;
    ctx->regs.x86.ebx = 0xaaaabeefu;
    ctx->regs.x86.ecx = 0x12340000u;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_memory_read_u16(ctx->memory, data_base + 8, &value16) == HB_OK);
    ASSERT(value16 == 0xbeefu);
    ASSERT(ctx->regs.x86.ecx == 0x1234beefu);
    ASSERT(ctx->regs.x86.eax == ((data_base & 0xffff0000u) | 0x7654u));
    ASSERT(ctx->regs.x86.eip == code_base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_rol_ror_basic) {
    uint8_t code[] = {
        0x48, 0xc1, 0xc1, 0x0d, /* rol $13, %rcx */
        0x48, 0xc1, 0xca, 0x07  /* ror $7, %rdx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 0x0000e01f8ff147c7ULL;
    ctx->regs.x64.rdx = 0x0123456789abcdefULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 0x1c03f1fe28f8e000ULL);
    ASSERT(ctx->regs.x64.rdx == 0xde02468acf13579bULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_rotate_flags_preserve_zf) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(0x8000000000000000ULL, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_ROL,
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
        hb_ir_imm(1, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ctx->pc = 0x1000;
    ctx->flags.zf = 1;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 1);
    ASSERT(ctx->flags.cf == 1);
    ASSERT(ctx->flags.of == 1);
    ASSERT(ctx->flags.zf == 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_test_cx_imm16_calc_security_cookie) {
    uint8_t code[] = {
        0x66, 0xf7, 0xc1, 0xff, 0xff,       /* test $0xffff, %cx */
        0x0f, 0x85, 0x54, 0xb1, 0x02, 0x00  /* jne ... */
    };
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x100001c41ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 2);
    ASSERT(d.op2.is_imm && d.op2.size == 2);
    ASSERT((uint16_t)d.op2.imm == 0xffffU);
    tests_passed++;
}

TEST(lift_x64_calc_cookie_guard_stops_at_jne) {
    uint8_t code[] = {
        0x48, 0xc1, 0xc1, 0x10,             /* rol $0x10, %rcx */
        0x66, 0xf7, 0xc1, 0xff, 0xff,       /* test $0xffff, %cx */
        0x0f, 0x85, 0x54, 0xb1, 0x02, 0x00, /* jne 0x10002cda0 */
        0xc2, 0x00, 0x00                    /* ret $0, must be next block */
    };
    uint64_t base = 0x100001c3dULL;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg != NULL);
    ASSERT(func->cfg->entry != NULL);
    ASSERT(func->cfg->entry->instr_count == 3);
    ASSERT(func->cfg->entry->instrs[0].op == HB_IR_ROL);
    ASSERT(func->cfg->entry->instrs[1].op == HB_IR_TEST);
    ASSERT(func->cfg->entry->instrs[1].guest_len == 5);
    ASSERT(func->cfg->entry->instrs[2].op == HB_IR_Jcc);
    ASSERT(func->cfg->entry->instrs[2].guest_addr == base + 9);
    ASSERT(func->cfg->entry->instrs[2].guest_len == 6);
    ASSERT(func->cfg->entry->instrs[2].target == 0x10002cda0ULL);

    hb_ir_func_destroy(func);
    tests_passed++;
}

static hb_cc_t fuzz_cc_for(hb_ir_op_t op, uint64_t count, int i) {
    static const hb_cc_t all[] = {
        HB_CC_E, HB_CC_NE, HB_CC_L, HB_CC_LE, HB_CC_G, HB_CC_GE,
        HB_CC_B, HB_CC_AE, HB_CC_BE, HB_CC_A, HB_CC_S, HB_CC_NS,
        HB_CC_O, HB_CC_NO
    };
    static const hb_cc_t no_of[] = {
        HB_CC_E, HB_CC_NE, HB_CC_B, HB_CC_AE, HB_CC_BE, HB_CC_A,
        HB_CC_S, HB_CC_NS
    };
    if ((op == HB_IR_SHL || op == HB_IR_SHR || op == HB_IR_SAR) && ((count & 0x3FULL) != 1)) {
        return no_of[(size_t)i % (sizeof(no_of) / sizeof(no_of[0]))];
    }
    return all[(size_t)i % (sizeof(all) / sizeof(all[0]))];
}

static void run_compare(hb_ir_func_t* func, uint64_t lhs, uint64_t* out_jit_rcx, uint64_t* out_interp_rcx) {
    hb_context_t* jit = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_context_t* interp = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    jit->pc = 0x1000;
    interp->pc = 0x1000;
    jit->regs.x64.rax = lhs;
    interp->regs.x64.rax = lhs;

    hb_exec_result_t oj, oi;
    hb_runtime_run(jit, func, HB_BACKEND_JIT, &oj);
    hb_runtime_run(interp, func, HB_BACKEND_INTERP, &oi);

    if (oj.result != oi.result) {
        fprintf(stderr, "COMPARE result mismatch: jit=%d interp=%d jit_fault=%d interp_fault=%d jit_reason=%s interp_reason=%s\n",
                oj.result, oi.result, oj.faulted, oi.faulted,
                oj.fault_reason ? oj.fault_reason : "",
                oi.fault_reason ? oi.fault_reason : "");
    }
    ASSERT(oj.result == oi.result);
    ASSERT(jit->regs.x64.rax == interp->regs.x64.rax);
    ASSERT(jit->regs.x64.rdx == interp->regs.x64.rdx);
    *out_jit_rcx = jit->regs.x64.rcx;
    *out_interp_rcx = interp->regs.x64.rcx;

    hb_context_destroy(jit);
    hb_context_destroy(interp);
}

typedef enum phase1_op_t {
    PHASE1_ADD,
    PHASE1_ADC,
    PHASE1_SUB,
    PHASE1_SBB,
    PHASE1_CMP,
    PHASE1_TEST,
    PHASE1_AND,
    PHASE1_OR,
    PHASE1_XOR,
    PHASE1_SHL,
    PHASE1_SHR,
    PHASE1_SAR
} phase1_op_t;

typedef struct phase1_expected_t {
    uint64_t rax;
    uint32_t flags_mask;
    bool cf, of, zf, sf, pf, af;
} phase1_expected_t;

static bool phase1_parity_even8(uint64_t value) {
    uint8_t v = (uint8_t)value;
    v ^= (uint8_t)(v >> 4);
    v &= 0x0f;
    return ((0x6996u >> v) & 1u) == 0;
}

static void phase1_fill_result_flags(phase1_expected_t* e, uint64_t result) {
    e->zf = (result == 0);
    e->sf = (result >> 63) != 0;
    e->pf = phase1_parity_even8(result);
}

static phase1_expected_t phase1_oracle(phase1_op_t op, uint64_t lhs, uint64_t rhs,
                                       uint8_t count, bool carry_in,
                                       const phase1_expected_t* initial) {
    phase1_expected_t e = *initial;
    uint64_t result = lhs;
    uint64_t eff_count = (uint64_t)count & 0x3f;
    const uint64_t sign = 0x8000000000000000ULL;
    const __int128 min_i64 = -((__int128)1 << 63);
    const __int128 max_i64 = (((__int128)1 << 63) - 1);

    e.rax = lhs;
    switch (op) {
    case PHASE1_ADD: {
        unsigned __int128 wide = (unsigned __int128)lhs + rhs;
        result = (uint64_t)wide;
        e.rax = result;
        e.cf = wide >> 64;
        e.of = ((~(lhs ^ rhs) & (lhs ^ result) & sign) != 0);
        e.af = ((lhs ^ rhs ^ result) & 0x10) != 0;
        e.flags_mask = HB_FLAG_BIT_ALL;
        phase1_fill_result_flags(&e, result);
        break;
    }
    case PHASE1_ADC: {
        uint64_t carry = carry_in ? 1 : 0;
        unsigned __int128 wide = (unsigned __int128)lhs + rhs + carry;
        __int128 signed_wide = (__int128)(int64_t)lhs + (int64_t)rhs + (int64_t)carry;
        result = (uint64_t)wide;
        e.rax = result;
        e.cf = wide >> 64;
        e.of = signed_wide < min_i64 || signed_wide > max_i64;
        e.af = ((lhs ^ rhs ^ result) & 0x10) != 0;
        e.flags_mask = HB_FLAG_BIT_ALL;
        phase1_fill_result_flags(&e, result);
        break;
    }
    case PHASE1_SUB:
    case PHASE1_CMP: {
        result = lhs - rhs;
        if (op == PHASE1_SUB) e.rax = result;
        else e.rax = lhs;
        e.cf = lhs < rhs;
        e.of = (((lhs ^ rhs) & (lhs ^ result) & sign) != 0);
        e.af = ((lhs ^ rhs ^ result) & 0x10) != 0;
        e.flags_mask = HB_FLAG_BIT_ALL;
        phase1_fill_result_flags(&e, result);
        break;
    }
    case PHASE1_SBB: {
        uint64_t borrow = carry_in ? 1 : 0;
        unsigned __int128 subtrahend = (unsigned __int128)rhs + borrow;
        __int128 signed_wide = (__int128)(int64_t)lhs - (int64_t)rhs - (int64_t)borrow;
        result = (uint64_t)((unsigned __int128)lhs - subtrahend);
        e.rax = result;
        e.cf = (unsigned __int128)lhs < subtrahend;
        e.of = signed_wide < min_i64 || signed_wide > max_i64;
        e.af = ((lhs ^ rhs ^ result) & 0x10) != 0;
        e.flags_mask = HB_FLAG_BIT_ALL;
        phase1_fill_result_flags(&e, result);
        break;
    }
    case PHASE1_TEST:
        result = lhs & rhs;
        e.rax = lhs;
        e.cf = false;
        e.of = false;
        e.flags_mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                       HB_FLAG_BIT_OF | HB_FLAG_BIT_PF;
        phase1_fill_result_flags(&e, result);
        break;
    case PHASE1_AND:
    case PHASE1_OR:
    case PHASE1_XOR:
        if (op == PHASE1_AND) result = lhs & rhs;
        else if (op == PHASE1_OR) result = lhs | rhs;
        else result = lhs ^ rhs;
        e.rax = result;
        e.cf = false;
        e.of = false;
        e.flags_mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                       HB_FLAG_BIT_OF | HB_FLAG_BIT_PF;
        phase1_fill_result_flags(&e, result);
        break;
    case PHASE1_SHL:
        if (!eff_count) break;
        result = lhs << eff_count;
        e.rax = result;
        e.cf = (lhs >> (64 - eff_count)) & 1;
        e.flags_mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF | HB_FLAG_BIT_PF;
        if (eff_count == 1) {
            e.of = (((result >> 63) & 1) != e.cf);
            e.flags_mask |= HB_FLAG_BIT_OF;
        }
        phase1_fill_result_flags(&e, result);
        break;
    case PHASE1_SHR:
        if (!eff_count) break;
        result = lhs >> eff_count;
        e.rax = result;
        e.cf = (lhs >> (eff_count - 1)) & 1;
        e.flags_mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF | HB_FLAG_BIT_PF;
        if (eff_count == 1) {
            e.of = (lhs >> 63) != 0;
            e.flags_mask |= HB_FLAG_BIT_OF;
        }
        phase1_fill_result_flags(&e, result);
        break;
    case PHASE1_SAR:
        if (!eff_count) break;
        result = (uint64_t)((int64_t)lhs >> eff_count);
        e.rax = result;
        e.cf = (lhs >> (eff_count - 1)) & 1;
        e.flags_mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF | HB_FLAG_BIT_PF;
        if (eff_count == 1) {
            e.of = false;
            e.flags_mask |= HB_FLAG_BIT_OF;
        }
        phase1_fill_result_flags(&e, result);
        break;
    }
    return e;
}

static int phase1_run_x64_bytes(const uint8_t* code, size_t len, uint64_t lhs,
                                uint64_t rhs, uint8_t count, bool carry_in,
                                uint32_t materialize_mask,
                                hb_context_t** out_ctx) {
    uint64_t base = (uint64_t)(uintptr_t)code;
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, len, base);
    hb_ir_func_t* func = NULL;
    hb_context_t* ctx = NULL;
    hb_exec_result_t out;

    if (!dec) {
        fprintf(stderr, "FAIL: phase1 decoder create failed\n");
        goto fail;
    }
    hb_result_t lift_status = hb_lift_func_x64(dec, &func);
    if (lift_status != HB_OK || !func) {
        fprintf(stderr, "FAIL: phase1 lift failed status=%d\n", lift_status);
        goto fail;
    }
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    if (!ctx) {
        fprintf(stderr, "FAIL: phase1 context create failed\n");
        goto fail;
    }
    ctx->pc = base;
    ctx->regs.x64.rip = base;
    ctx->regs.x64.rax = lhs;
    ctx->regs.x64.rbx = rhs;
    ctx->regs.x64.rcx = count;
    ctx->flags.cf = carry_in;
    ctx->flags.of = true;
    ctx->flags.zf = false;
    ctx->flags.sf = true;
    ctx->flags.pf = false;
    ctx->flags.af = true;
    hb_result_t run_status = hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out);
    if (run_status != HB_OK || out.result != HB_OK) {
        fprintf(stderr, "FAIL: phase1 runtime failed status=%d result=%d pc=%llx\n",
                run_status, out.result, (unsigned long long)ctx->pc);
        goto fail;
    }
    hb_result_t flags_status = hb_lazy_flags_materialize(ctx, materialize_mask);
    if (flags_status != HB_OK) {
        fprintf(stderr, "FAIL: phase1 materialize failed status=%d\n", flags_status);
        goto fail;
    }
    hb_decoder_destroy(dec);
    hb_ir_func_destroy(func);
    *out_ctx = ctx;
    return 1;

fail:
    if (dec) hb_decoder_destroy(dec);
    if (func) hb_ir_func_destroy(func);
    if (ctx) hb_context_destroy(ctx);
    return 0;
}

static void phase1_assert_flags(const char* name, uint64_t lhs, uint64_t rhs,
                                uint8_t count, bool carry, const phase1_expected_t* e,
                                const hb_context_t* ctx) {
    if (ctx->regs.x64.rax != e->rax ||
        ((e->flags_mask & HB_FLAG_BIT_CF) && ctx->flags.cf != e->cf) ||
        ((e->flags_mask & HB_FLAG_BIT_OF) && ctx->flags.of != e->of) ||
        ((e->flags_mask & HB_FLAG_BIT_ZF) && ctx->flags.zf != e->zf) ||
        ((e->flags_mask & HB_FLAG_BIT_SF) && ctx->flags.sf != e->sf) ||
        ((e->flags_mask & HB_FLAG_BIT_PF) && ctx->flags.pf != e->pf) ||
        ((e->flags_mask & HB_FLAG_BIT_AF) && ctx->flags.af != e->af)) {
        fprintf(stderr,
                "FAIL: phase1 oracle %s rax=%llx/%llx flags got cf=%d of=%d zf=%d sf=%d pf=%d af=%d expected cf=%d of=%d zf=%d sf=%d pf=%d af=%d mask=0x%x\n",
                name, (unsigned long long)ctx->regs.x64.rax, (unsigned long long)e->rax,
                ctx->flags.cf, ctx->flags.of, ctx->flags.zf, ctx->flags.sf, ctx->flags.pf, ctx->flags.af,
                e->cf, e->of, e->zf, e->sf, e->pf, e->af, e->flags_mask);
        fprintf(stderr, "FAIL: phase1 input lhs=%llx rhs=%llx count=%u carry=%d\n",
                (unsigned long long)lhs, (unsigned long long)rhs, count, carry ? 1 : 0);
        tests_failed++;
    }
}

TEST(phase1_x64_arith_logic_shift_flags_oracle_fuzzer) {
    static const struct {
        phase1_op_t op;
        const char* name;
        uint8_t code[4];
        size_t len;
    } ops[] = {
        {PHASE1_ADD,  "add",  {0x48, 0x01, 0xd8}, 3}, /* add %rbx,%rax */
        {PHASE1_ADC,  "adc",  {0x48, 0x11, 0xd8}, 3}, /* adc %rbx,%rax */
        {PHASE1_SUB,  "sub",  {0x48, 0x29, 0xd8}, 3}, /* sub %rbx,%rax */
        {PHASE1_SBB,  "sbb",  {0x48, 0x19, 0xd8}, 3}, /* sbb %rbx,%rax */
        {PHASE1_CMP,  "cmp",  {0x48, 0x39, 0xd8}, 3}, /* cmp %rbx,%rax */
        {PHASE1_TEST, "test", {0x48, 0x85, 0xd8}, 3}, /* test %rbx,%rax */
        {PHASE1_AND,  "and",  {0x48, 0x21, 0xd8}, 3}, /* and %rbx,%rax */
        {PHASE1_OR,   "or",   {0x48, 0x09, 0xd8}, 3}, /* or %rbx,%rax */
        {PHASE1_XOR,  "xor",  {0x48, 0x31, 0xd8}, 3}, /* xor %rbx,%rax */
        {PHASE1_SHL,  "shl",  {0x48, 0xd3, 0xe0}, 3}, /* shl %cl,%rax */
        {PHASE1_SHR,  "shr",  {0x48, 0xd3, 0xe8}, 3}, /* shr %cl,%rax */
        {PHASE1_SAR,  "sar",  {0x48, 0xd3, 0xf8}, 3}, /* sar %cl,%rax */
    };
    uint64_t values[] = {
        0, 1, 2, 0x0f, 0x10, 0x7fffffffffffffffULL,
        0x8000000000000000ULL, 0xffffffffffffffffULL,
        0x55aa55aa55aa55aaULL
    };
    uint8_t counts[] = {0, 1, 2, 7, 31, 32, 63, 64, 65};
    phase1_expected_t initial = {
        .rax = 0, .flags_mask = HB_FLAG_BIT_ALL,
        .cf = true, .of = true, .zf = false, .sf = true, .pf = false, .af = true
    };

    for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); oi++) {
        for (size_t vi = 0; vi < sizeof(values) / sizeof(values[0]); vi++) {
            for (size_t ri = 0; ri < sizeof(values) / sizeof(values[0]); ri++) {
                uint64_t lhs = values[vi] ^ (uint64_t)(oi * 0x111111111111111ULL);
                uint64_t rhs = values[ri] + (uint64_t)(vi * 17 + oi);
                uint8_t count = counts[(vi + ri + oi) % (sizeof(counts) / sizeof(counts[0]))];
                bool carry = ((vi + ri + oi) & 1) != 0;
                hb_context_t* ctx = NULL;
                phase1_expected_t initial_iter = initial;
                initial_iter.cf = carry;
                phase1_expected_t expected = phase1_oracle(ops[oi].op, lhs, rhs, count, carry, &initial_iter);
                expected.rax = (ops[oi].op == PHASE1_CMP || ops[oi].op == PHASE1_TEST) ? lhs : expected.rax;
                if (!phase1_run_x64_bytes(ops[oi].code, ops[oi].len, lhs, rhs, count, carry,
                                          expected.flags_mask, &ctx)) {
                    fprintf(stderr,
                            "FAIL: phase1 run %s lhs=%llx rhs=%llx count=%u carry=%d\n",
                            ops[oi].name, (unsigned long long)lhs, (unsigned long long)rhs,
                            count, carry ? 1 : 0);
                    ASSERT(0);
                }
                phase1_assert_flags(ops[oi].name, lhs, rhs, count, carry, &expected, ctx);
                hb_context_destroy(ctx);
                ASSERT(tests_failed == 0);
            }
        }
    }
    tests_passed++;
}

TEST(diff_lazy_flags_jcc_fuzzer) {
    hb_ir_op_t ops[] = {HB_IR_ADD, HB_IR_SUB, HB_IR_CMP, HB_IR_TEST, HB_IR_SHL, HB_IR_SHR, HB_IR_SAR};
    uint64_t values[] = {0, 1, ~0ULL, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL};
    for (size_t i = 0; i < 84; i++) {
        hb_ir_op_t op = ops[i % (sizeof(ops) / sizeof(ops[0]))];
        uint64_t lhs = values[i % (sizeof(values) / sizeof(values[0]))] ^ (uint64_t)(i * 0x10101U);
        uint64_t rhs = values[(i + 3) % (sizeof(values) / sizeof(values[0]))] + i;
        hb_cc_t cc = fuzz_cc_for(op, rhs, (int)i);

        hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
        hb_ir_block_t* a = hb_ir_block_create(0, 0x1000);
        hb_ir_block_t* b = hb_ir_block_create(1, 0x1010);
        hb_ir_block_t* c = hb_ir_block_create(2, 0x1020);
        hb_ir_cfg_add_block(func->cfg, a);
        hb_ir_cfg_add_block(func->cfg, b);
        hb_ir_cfg_add_block(func->cfg, c);
        func->cfg->entry = a;

        hb_ir_builder_t* builder = hb_ir_builder_create(func);
        hb_ir_builder_set_block(builder, a);
        hb_ir_emit_mov(builder, hb_ir_reg(HB_REG_RBX, HB_SIZE_64), hb_ir_imm((int64_t)rhs, HB_SIZE_64));
        if (op == HB_IR_CMP) hb_ir_emit_cmp(builder, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        else if (op == HB_IR_TEST) hb_ir_emit_test(builder, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        else hb_ir_emit_binop(builder, op, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        hb_ir_instr_t* jcc = hb_ir_emit_jcc(builder, cc, 0x1020);
        jcc->guest_addr = 0x100A;
        jcc->guest_len = 6;
        hb_ir_builder_set_block(builder, b);
        hb_ir_emit_mov(builder, hb_ir_reg(HB_REG_RCX, HB_SIZE_64), hb_ir_imm(0, HB_SIZE_64));
        hb_ir_builder_set_block(builder, c);
        hb_ir_emit_mov(builder, hb_ir_reg(HB_REG_RCX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
        hb_ir_builder_destroy(builder);

        uint64_t jit_rcx = 0, interp_rcx = 0;
        run_compare(func, lhs, &jit_rcx, &interp_rcx);
        if (jit_rcx != interp_rcx) {
            fprintf(stderr, "JCC fuzz mismatch i=%zu op=%d cc=%d lhs=%llu rhs=%llu jit_rcx=%llu interp_rcx=%llu\n",
                    i, op, cc, (unsigned long long)lhs, (unsigned long long)rhs,
                    (unsigned long long)jit_rcx, (unsigned long long)interp_rcx);
        }
        ASSERT(jit_rcx == interp_rcx);
        hb_ir_func_destroy(func);
    }
    tests_passed++;
}

TEST(diff_lazy_flags_setcc_cmovcc_fuzzer) {
    hb_ir_op_t ops[] = {HB_IR_ADD, HB_IR_SUB, HB_IR_CMP, HB_IR_TEST, HB_IR_SHL, HB_IR_SHR, HB_IR_SAR};
    for (size_t i = 0; i < 84; i++) {
        hb_ir_op_t op = ops[i % (sizeof(ops) / sizeof(ops[0]))];
        uint64_t lhs = ((uint64_t)i << 56) ^ (0xF0F0F0F0ULL + i);
        uint64_t rhs = (i * 17U) ^ 0x8000000000000001ULL;
        hb_cc_t cc = fuzz_cc_for(op, rhs, (int)(i + 5));

        hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
        hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
        hb_ir_cfg_add_block(func->cfg, blk);
        func->cfg->entry = blk;
        hb_ir_builder_t* b = hb_ir_builder_create(func);
        hb_ir_builder_set_block(b, blk);
        hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RBX, HB_SIZE_64), hb_ir_imm((int64_t)rhs, HB_SIZE_64));
        hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RDX, HB_SIZE_64), hb_ir_imm((int64_t)0xAA55AA55AA55AA55ULL, HB_SIZE_64));
        if (op == HB_IR_CMP) hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        else if (op == HB_IR_TEST) hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        else hb_ir_emit_binop(b, op, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        hb_ir_emit_setcc(b, cc, hb_ir_reg(HB_REG_RCX, HB_SIZE_8));
        hb_ir_emit_cmovcc(b, cc, hb_ir_reg(HB_REG_RDX, HB_SIZE_64), hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
        hb_ir_builder_destroy(b);

        uint64_t jit_rcx = 0, interp_rcx = 0;
        run_compare(func, lhs, &jit_rcx, &interp_rcx);
        if (jit_rcx != interp_rcx) {
            fprintf(stderr, "SETCC fuzz mismatch i=%zu op=%d cc=%d lhs=%llu rhs=%llu jit_rcx=%llu interp_rcx=%llu\n",
                    i, op, cc, (unsigned long long)lhs, (unsigned long long)rhs,
                    (unsigned long long)jit_rcx, (unsigned long long)interp_rcx);
        }
        ASSERT(jit_rcx == interp_rcx);
        hb_ir_func_destroy(func);
    }
    tests_passed++;
}

TEST(decode_adc_sbb_lahf_sahf) {
    hb_decoded_t d;
    uint8_t adc[] = {0x48, 0x83, 0xd0, 0x01}; /* adc rax, 1 */
    ASSERT(hb_decode_x64(adc, sizeof(adc), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADC);
    ASSERT(d.reads_flags);
    ASSERT(d.op1.reg == HB_REG_RAX);
    ASSERT(d.op2.imm == 1);

    uint8_t sbb[] = {0x48, 0x19, 0x08}; /* sbb [rax], rcx */
    ASSERT(hb_decode_x64(sbb, sizeof(sbb), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SBB);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op2.reg == HB_REG_RCX);

    uint8_t lahf[] = {0x9f};
    ASSERT(hb_decode_x64(lahf, sizeof(lahf), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LAHF);
    uint8_t sahf[] = {0x9e};
    ASSERT(hb_decode_x64(sahf, sizeof(sahf), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SAHF);
    tests_passed++;
}

TEST(jit_adc_sbb_carry_chain) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_ADC, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(2, HB_SIZE_64));
    hb_ir_emit_binop(b, HB_IR_SBB, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->flags.cf = true; /* ADC must consume existing carry. */
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 3);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_partial_mov_preserves_upper_bits) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8), hb_ir_reg(HB_REG_RDX, HB_SIZE_8));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->regs.x64.rax = 0xdeadbeefcafeba00ULL;
    ctx->regs.x64.rdx = 0x77;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0xdeadbeefcafeba77ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_mov_family) {
    uint8_t mem[16] = {0x78, 0x56, 0x34, 0x12, 0, 0xcc, 0, 0};
    hb_ir_func_t* func = hb_ir_func_create(0x4200, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4200);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* mov_al = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                           hb_ir_reg(HB_REG_RDX, HB_SIZE_8));
    hb_ir_instr_t* mov_bx = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RBX, HB_SIZE_16),
                                           hb_ir_imm(0xabcd, HB_SIZE_16));
    hb_ir_instr_t* mov_ecx = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RCX, HB_SIZE_32),
                                            hb_ir_imm(0x89abcdefu, HB_SIZE_32));
    hb_ir_instr_t* mov_r9d = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_R9, HB_SIZE_32),
                                            hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_32));
    hb_ir_instr_t* mov_mem8 = hb_ir_emit_mov(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 4, HB_SIZE_8),
                                              hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* mov_mem8_imm = hb_ir_emit_mov(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 5, HB_SIZE_8),
                                                  hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* mov_mem16 = hb_ir_emit_mov(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 6, HB_SIZE_16),
                                               hb_ir_imm(0x55aa, HB_SIZE_16));
    ASSERT(mov_al && mov_bx && mov_ecx && mov_r9d && mov_mem8 && mov_mem8_imm && mov_mem16);
    mov_al->guest_addr = 0x4200; mov_al->guest_len = 2;
    mov_bx->guest_addr = 0x4202; mov_bx->guest_len = 4;
    mov_ecx->guest_addr = 0x4206; mov_ecx->guest_len = 5;
    mov_r9d->guest_addr = 0x420b; mov_r9d->guest_len = 4;
    mov_mem8->guest_addr = 0x420f; mov_mem8->guest_len = 4;
    mov_mem8_imm->guest_addr = 0x4213; mov_mem8_imm->guest_len = 4;
    mov_mem16->guest_addr = 0x4217; mov_mem16->guest_len = 7;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem, sizeof(mem),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4200;
    ctx->regs.x64.rax = 0xdeadbeefcafeba00ULL;
    ctx->regs.x64.rbx = 0x1111222233334444ULL;
    ctx->regs.x64.rcx = UINT64_MAX;
    ctx->regs.x64.rdx = 0x77;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)mem;
    ctx->regs.x64.r9 = UINT64_MAX;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rax == 0xdeadbeefcafeba77ULL);
    ASSERT(ctx->regs.x64.rbx == 0x111122223333abcdULL);
    ASSERT(ctx->regs.x64.rcx == 0x89abcdefULL);
    ASSERT(ctx->regs.x64.r9 == 0x12345678ULL);
    ASSERT(mem[4] == 0x77);
    ASSERT(mem[5] == 0x00);
    ASSERT(mem[6] == 0xaa && mem[7] == 0x55);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_store_imm_compact_family) {
    uint8_t mem[16];
    memset(mem, 0xcc, sizeof(mem));
    hb_ir_func_t* func = hb_ir_func_create(0x4240, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4240);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store8 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                             hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* store16 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 2, HB_SIZE_16),
                                              hb_ir_imm(0x55aa, HB_SIZE_16));
    hb_ir_instr_t* store32 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 4, HB_SIZE_32),
                                              hb_ir_imm(0x89abcdefu, HB_SIZE_32));
    ASSERT(store8 && store16 && store32);
    store8->guest_addr = 0x4240; store8->guest_len = 4;
    store16->guest_addr = 0x4244; store16->guest_len = 5;
    store32->guest_addr = 0x4249; store32->guest_len = 7;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem, sizeof(mem),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4240;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)mem;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(mem[0] == 0x00);
    ASSERT(mem[2] == 0xaa && mem[3] == 0x55);
    ASSERT(mem[4] == 0xef && mem[5] == 0xcd && mem[6] == 0xab && mem[7] == 0x89);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 112);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_extend_family) {
    uint8_t mem[8] = {0xf0, 0x7f, 0xfe, 0x80, 0, 0, 0, 0};
    hb_ir_func_t* func = hb_ir_func_create(0x4500, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4500);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* movzx_reg = hb_ir_emit_unop(b, HB_IR_ZERO_EXTEND,
                                               hb_ir_reg(HB_REG_RDX, HB_SIZE_32),
                                               hb_ir_reg(HB_REG_RBX, HB_SIZE_8));
    hb_ir_instr_t* movsx_reg = hb_ir_emit_unop(b, HB_IR_SIGN_EXTEND,
                                               hb_ir_reg(HB_REG_RCX, HB_SIZE_32),
                                               hb_ir_reg(HB_REG_RBX, HB_SIZE_8));
    hb_ir_instr_t* movzx_mem = hb_ir_emit_unop(b, HB_IR_ZERO_EXTEND,
                                               hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                               hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 2, HB_SIZE_16));
    hb_ir_instr_t* movsx_mem64 = hb_ir_emit_unop(b, HB_IR_SIGN_EXTEND,
                                                 hb_ir_reg(HB_REG_R10, HB_SIZE_64),
                                                 hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* movsx_mem32 = hb_ir_emit_unop(b, HB_IR_SIGN_EXTEND,
                                                 hb_ir_reg(HB_REG_R11, HB_SIZE_32),
                                                 hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_8));
    ASSERT(movzx_reg && movsx_reg && movzx_mem && movsx_mem64 && movsx_mem32);
    movzx_reg->guest_addr = 0x4500; movzx_reg->guest_len = 3;
    movsx_reg->guest_addr = 0x4503; movsx_reg->guest_len = 3;
    movzx_mem->guest_addr = 0x4506; movzx_mem->guest_len = 4;
    movsx_mem64->guest_addr = 0x450a; movsx_mem64->guest_len = 4;
    movsx_mem32->guest_addr = 0x450e; movsx_mem32->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem, sizeof(mem),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4500;
    ctx->regs.x64.rbx = 0x80;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)mem;
    ctx->regs.x64.rdx = UINT64_MAX;
    ctx->regs.x64.rcx = UINT64_MAX;
    ctx->regs.x64.r9 = UINT64_MAX;
    ctx->regs.x64.r10 = 0;
    ctx->regs.x64.r11 = UINT64_MAX;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rdx == 0x80);
    ASSERT(ctx->regs.x64.rcx == 0xffffff80ULL);
    ASSERT(ctx->regs.x64.r9 == 0x80fe);
    ASSERT(ctx->regs.x64.r10 == 0xfffffffffffffff0ULL);
    ASSERT(ctx->regs.x64.r11 == 0xfffffff0ULL);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 180);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_xmm_move_family) {
    uint64_t src[2] = {0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
    uint64_t dst_store[2] = {0, 0};
    uint64_t dst_mov[2] = {0, 0};
    hb_ir_func_t* func = hb_ir_func_create(0x4600, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4600);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_XMM1, HB_SIZE_128),
                                          hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_128));
    hb_ir_instr_t* mov_reg = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_XMM2, HB_SIZE_128),
                                            hb_ir_reg(HB_REG_XMM1, HB_SIZE_128));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_128),
                                            hb_ir_reg(HB_REG_XMM2, HB_SIZE_128));
    hb_ir_instr_t* mov_load = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_XMM3, HB_SIZE_128),
                                             hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_128));
    hb_ir_instr_t* mov_store = hb_ir_emit_mov(b, hb_ir_mem(HB_REG_R10, HB_REG_COUNT, 1, 0, HB_SIZE_128),
                                              hb_ir_reg(HB_REG_XMM3, HB_SIZE_128));
    ASSERT(load && mov_reg && store && mov_load && mov_store);
    load->guest_addr = 0x4600; load->guest_len = 7;
    mov_reg->guest_addr = 0x4607; mov_reg->guest_len = 3;
    store->guest_addr = 0x460a; store->guest_len = 7;
    mov_load->guest_addr = 0x4611; mov_load->guest_len = 7;
    mov_store->guest_addr = 0x4618; mov_store->guest_len = 7;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst_store, sizeof(dst_store),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst_mov, sizeof(dst_mov),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4600;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst_store;
    ctx->regs.x64.r10 = (uint64_t)(uintptr_t)dst_mov;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.xmm[1][0] == src[0] && ctx->regs.x64.xmm[1][1] == src[1]);
    ASSERT(ctx->regs.x64.xmm[2][0] == src[0] && ctx->regs.x64.xmm[2][1] == src[1]);
    ASSERT(ctx->regs.x64.xmm[3][0] == src[0] && ctx->regs.x64.xmm[3][1] == src[1]);
    ASSERT(dst_store[0] == src[0] && dst_store[1] == src[1]);
    ASSERT(dst_mov[0] == src[0] && dst_mov[1] == src[1]);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_neg_al_sbb_mask_notepadpp_mode_parser) {
    for (int input = 0; input <= 1; input++) {
        hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
        ASSERT(func != NULL);
        hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
        ASSERT(blk != NULL);
        hb_ir_cfg_add_block(func->cfg, blk);
        func->cfg->entry = blk;
        hb_ir_builder_t* b = hb_ir_builder_create(func);
        hb_ir_builder_set_block(b, blk);
        hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8), hb_ir_reg(HB_REG_RDX, HB_SIZE_8));
        hb_ir_emit_unop(b, HB_IR_NEG, hb_ir_reg(HB_REG_RAX, HB_SIZE_8), hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
        hb_ir_emit_binop(b, HB_IR_SBB, hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                          hb_ir_reg(HB_REG_RCX, HB_SIZE_64), hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
        hb_ir_emit_binop(b, HB_IR_AND, hb_ir_reg(HB_REG_RCX, HB_SIZE_32),
                          hb_ir_reg(HB_REG_RCX, HB_SIZE_32), hb_ir_imm(2, HB_SIZE_32));
        hb_ir_builder_destroy(b);

        hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
        ASSERT(ctx != NULL);
        ctx->pc = 0x1000;
        ctx->regs.x64.rax = 0xdeadbeefcafeba00ULL;
        ctx->regs.x64.rcx = 0x12345678;
        ctx->regs.x64.rdx = (uint64_t)input;

        hb_exec_result_t out;
        ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
        ASSERT(out.result == HB_OK);
        ASSERT(ctx->regs.x64.rcx == (input ? 2ULL : 0ULL));
        ASSERT((ctx->regs.x64.rax & 0xffffffffffffff00ULL) == 0xdeadbeefcafeba00ULL);
        hb_context_destroy(ctx);
        hb_ir_func_destroy(func);
    }
    tests_passed++;
}

TEST(jit_lahf_sahf_roundtrip) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit(b, HB_IR_LAHF);
    hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(0x9300, HB_SIZE_64));
    hb_ir_emit(b, HB_IR_SAHF);
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x1000;
    ctx->flags.sf = true;
    ctx->flags.zf = true;
    ctx->flags.pf = true;
    ctx->flags.cf = true;
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.sf);
    ASSERT(!ctx->flags.zf);
    ASSERT(ctx->flags.af);
    ASSERT(!ctx->flags.pf);
    ASSERT(ctx->flags.cf);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_setcc_cmovcc_memory_operands) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;
    hb_ir_builder_t* b = hb_ir_builder_create(func);
    hb_ir_builder_set_block(b, blk);
    hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(7, HB_SIZE_64));
    hb_ir_emit_setcc(b, HB_CC_E, hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 0, HB_SIZE_8));
    hb_ir_emit_cmovcc(b, HB_CC_E, hb_ir_reg(HB_REG_RCX, HB_SIZE_64), hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0x10000);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_setup_heap(ctx->memory, 0, 4096) == HB_OK);
    ctx->pc = 0x1000;
    ctx->regs.x64.rax = 7;
    ctx->regs.x64.rbx = ctx->memory->heap_base;
    ASSERT(hb_memory_write_u64(ctx->memory, ctx->memory->heap_base + 8, 0xfeedfacecafebeefULL) == HB_OK);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint8_t setcc = 0;
    ASSERT(hb_memory_read_u8(ctx->memory, ctx->memory->heap_base, &setcc) == HB_OK);
    ASSERT(setcc == 1);
    ASSERT(ctx->regs.x64.rcx == 0xfeedfacecafebeefULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movaps_xmm_store) {
    uint8_t code[] = {0x0f, 0x29, 0xb4, 0x24, 0xf0, 0x06, 0x00, 0x00};
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), 0x1000);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);

    uint8_t stack_buf[4096] __attribute__((aligned(16)));
    memset(stack_buf, 0, sizeof(stack_buf));
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack_buf, sizeof(stack_buf),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = 0x1000;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)stack_buf;
    ctx->regs.x64.xmm[6][0] = 0x1122334455667788ULL;
    ctx->regs.x64.xmm[6][1] = 0x99aabbccddeeff00ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint64_t stored[2] = {0, 0};
    memcpy(stored, stack_buf + 0x6f0, sizeof(stored));
    ASSERT(stored[0] == 0x1122334455667788ULL);
    ASSERT(stored[1] == 0x99aabbccddeeff00ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_rip_load_r12_indirect_call) {
    struct {
        uint8_t code[32];
        uint64_t iat_slot;
    } block __attribute__((aligned(16)));
    memset(&block, 0xcc, sizeof(block));

    uint64_t base = (uint64_t)(uintptr_t)block.code;
    uint64_t iat = (uint64_t)(uintptr_t)&block.iat_slot;
    int32_t disp = (int32_t)(iat - (base + 7));
    uint64_t target = 0x6f000000e390ULL;

    block.code[0] = 0x4c; /* movq disp32(%rip), %r12 */
    block.code[1] = 0x8b;
    block.code[2] = 0x25;
    memcpy(&block.code[3], &disp, sizeof(disp));
    block.code[7] = 0x31; /* xor %ecx,%ecx */
    block.code[8] = 0xc9;
    block.code[9] = 0x41; /* call *%r12 */
    block.code[10] = 0xff;
    block.code[11] = 0xd4;
    block.iat_slot = target;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, 12, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);

    uint8_t stack_buf[256] __attribute__((aligned(16)));
    memset(stack_buf, 0, sizeof(stack_buf));
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)stack_buf, sizeof(stack_buf),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)(stack_buf + sizeof(stack_buf));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.r12 == target);
    ASSERT(ctx->pc == target);
    uint64_t ret_addr = 0;
    ASSERT(hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &ret_addr) == HB_OK);
    ASSERT(ret_addr == base + 12);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_notepad_scalar_sse_sequence) {
    struct {
        uint8_t code[32];
        double scale;
    } block __attribute__((aligned(16)));
    memset(&block, 0xcc, sizeof(block));

    uint64_t base = (uint64_t)(uintptr_t)block.code;
    uint64_t scale_addr = (uint64_t)(uintptr_t)&block.scale;
    int32_t disp = (int32_t)(scale_addr - (base + 12));

    block.code[0] = 0xf2; /* cvtsi2sd %eax, %xmm0 */
    block.code[1] = 0x0f;
    block.code[2] = 0x2a;
    block.code[3] = 0xc0;
    block.code[4] = 0xf2; /* mulsd disp32(%rip), %xmm0 */
    block.code[5] = 0x0f;
    block.code[6] = 0x59;
    block.code[7] = 0x05;
    memcpy(&block.code[8], &disp, sizeof(disp));
    block.code[12] = 0xf2; /* cvttsd2si %xmm0, %ebp */
    block.code[13] = 0x0f;
    block.code[14] = 0x2c;
    block.code[15] = 0xe8;
    block.scale = 0.75;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, 16, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 768;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rbp == 576);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_movd_xmm_r32_calc) {
    uint8_t code[] = {0x66, 0x0f, 0x6e, 0xc0}; /* movd %eax, %xmm0 */
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1000, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX);
    ASSERT(d.op2.size == 4);
    tests_passed++;
}

TEST(interp_x64_movd_r32_xmm_notepadpp) {
    uint8_t code[] = {0x66, 0x0f, 0x7e, 0xc8}; /* movd %xmm1, %eax */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[1][0] = 0x1122334455667788ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x55667788ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movq_r64_xmm_notepadpp_open_pack) {
    uint8_t code[] = {
        0x66, 0x48, 0x0f, 0x7e, 0xc2, /* movq %xmm0, %rdx */
        0x48, 0xc1, 0xea, 0x20        /* shr $0x20, %rdx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdx = 0x1111111111111111ULL;
    ctx->regs.x64.xmm[0][0] = 0x8000000000000000ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0x80000000ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movq_xmm_r64_rexw_roundtrip) {
    uint8_t code[] = {0x66, 0x48, 0x0f, 0x6e, 0xc1}; /* movq %rcx, %xmm0 */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RCX);
    ASSERT(d.op2.size == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 0x1122334455667788ULL;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0x1122334455667788ULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movq_d6_xmm_store_family_notepadpp) {
    uint8_t reg_code[] = {0x66, 0x0f, 0xd6, 0xc1}; /* movq %xmm0, %xmm1 */
    uint64_t base = (uint64_t)(uintptr_t)reg_code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(reg_code, sizeof(reg_code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, reg_code, sizeof(reg_code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)reg_code, sizeof(reg_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x64.xmm[0][1] = 0x99aabbccddeeff00ULL;
    ctx->regs.x64.xmm[1][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[1][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[1][0] == 0x1122334455667788ULL);
    ASSERT(ctx->regs.x64.xmm[1][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t mem_code[] = {0x66, 0x0f, 0xd6, 0x00}; /* movq %xmm0, (%rax) */
    base = (uint64_t)(uintptr_t)mem_code;
    ASSERT(hb_decode_x64(mem_code, sizeof(mem_code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.op1.is_mem && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    dec = hb_decoder_create(HB_ARCH_X64, mem_code, sizeof(mem_code), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem_code, sizeof(mem_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, 0, 0x1000, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    hb_gva_t dst = ctx->memory->regions->base;
    ctx->pc = base;
    ctx->regs.x64.rax = dst;
    ctx->regs.x64.xmm[0][0] = 0x8877665544332211ULL;
    ctx->regs.x64.xmm[0][1] = 0x0102030405060708ULL;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint64_t stored = 0;
    ASSERT(hb_memory_read(ctx->memory, dst, &stored, sizeof(stored)) == HB_OK);
    ASSERT(stored == 0x8877665544332211ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movq_f3_xmm_load_family_smoke_helper) {
    uint8_t disp_code[] = {
        0xf3, 0x0f, 0x7e, 0xb4, 0x24, 0xb8, 0x01, 0x00, 0x00
    }; /* movq 0x1b8(%rsp), %xmm6 */
    uint64_t base = (uint64_t)(uintptr_t)disp_code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(disp_code, sizeof(disp_code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 9);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM6);
    ASSERT(d.op2.is_mem && d.op2.mem.base == HB_REG_RSP && d.op2.mem.disp == 0x1b8);
    ASSERT(d.op2.size == 8);

    uint8_t mem_code[] = {0xf3, 0x0f, 0x7e, 0x00}; /* movq (%rax), %xmm0 */
    base = (uint64_t)(uintptr_t)mem_code;
    ASSERT(hb_decode_x64(mem_code, sizeof(mem_code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_mem && d.op2.size == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, mem_code, sizeof(mem_code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem_code, sizeof(mem_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, 0, 0x1000, HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    hb_gva_t src_addr = ctx->memory->regions->base;
    uint64_t src = 0x0123456789abcdefULL;
    ASSERT(hb_memory_write(ctx->memory, src_addr, &src, sizeof(src)) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = src_addr;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == src);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t reg_code[] = {0xf3, 0x0f, 0x7e, 0xc1}; /* movq %xmm1, %xmm0 */
    base = (uint64_t)(uintptr_t)reg_code;
    ASSERT(hb_decode_x64(reg_code, sizeof(reg_code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);

    dec = hb_decoder_create(HB_ARCH_X64, reg_code, sizeof(reg_code), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)reg_code, sizeof(reg_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;
    ctx->regs.x64.xmm[1][0] = 0xfedcba9876543210ULL;
    ctx->regs.x64.xmm[1][1] = 0x0123456789abcdefULL;

    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0xfedcba9876543210ULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movabs_r11_imm64_notepadpp_strcmp) {
    uint8_t code[] = {
        0x49, 0xbb, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
        0x49, 0xba, 0xff, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == 10);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_R11);
    ASSERT(d.op1.size == 8);
    ASSERT(d.op2.is_imm && (uint64_t)d.op2.imm == 0x8080808080808080ULL);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.r11 == 0x8080808080808080ULL);
    ASSERT(ctx->regs.x64.r10 == 0xfefefefefefefeffULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_addr32_lea_notepadpp_strcmp) {
    uint8_t code[] = {0x67, 0x8d, 0x04, 0x0a}; /* lea eax, [edx+ecx] */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LEA);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 4);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.rdx = 0xffffffff00000010ULL;
    ctx->regs.x64.rcx = 0xffffffff00000020ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x30ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_bsr_r64_notepadpp) {
    uint8_t code[] = {
        0x48, 0x0f, 0xbd, 0xc8, /* bsr %rax, %rcx */
        0x48, 0x0f, 0xbd, 0xd3  /* bsr %rbx, %rdx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSR);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX);

    uint8_t lzcnt_code[] = {0xf3, 0x48, 0x0f, 0xbd, 0xc8}; /* lzcnt %rax, %rcx */
    ASSERT(hb_decode_x64(lzcnt_code, sizeof(lzcnt_code), (uint64_t)(uintptr_t)lzcnt_code, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_LZCNT);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 8);

    uint8_t bsf_code[] = {0x0f, 0xbc, 0xc8}; /* bsf %eax, %ecx */
    ASSERT(hb_decode_x64(bsf_code, sizeof(bsf_code), (uint64_t)(uintptr_t)bsf_code, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSF);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX && d.op2.size == 4);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x1000;
    ctx->regs.x64.rbx = 0;
    ctx->regs.x64.rdx = 0xfeedface;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 12);
    ASSERT(ctx->regs.x64.rdx == 0xfeedface);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_bsf_zero_preserves_cmovne_guard) {
    uint8_t code[] = {
        0x0f, 0xbc, 0xc8, /* bsf %eax, %ecx */
        0x0f, 0x45, 0xd1  /* cmovne %ecx, %edx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = 0x12345678;
    ctx->regs.x64.rdx = 0x55;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0x55);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_bsf_zero_preserves_cmovne_guard) {
    uint8_t code[] = {
        0x0f, 0xbc, 0xc8, /* bsf %eax, %ecx */
        0x0f, 0x45, 0xd1  /* cmovne %ecx, %edx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = 0x12345678;
    ctx->regs.x64.rdx = 0x55;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0x55);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_scalar_load_width_family) {
    uint8_t code[] = {
        0x8a, 0x03,             /* movb (%rbx), %al */
        0x66, 0x8b, 0x11,       /* movw (%rcx), %dx */
        0x8b, 0x75, 0x00        /* movl 0(%rbp), %esi */
    };
    uint8_t byte_value = 0xab;
    uint16_t word_value = 0xcdef;
    uint32_t dword_value = 0x12345678;
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&byte_value, sizeof(byte_value),
                         HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&word_value, sizeof(word_value),
                         HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&dword_value, sizeof(dword_value),
                         HB_PERM_READ) == HB_OK);

    ctx->pc = base;
    ctx->regs.x64.rax = 0x1122334455667700ULL;
    ctx->regs.x64.rdx = 0x8877665544330000ULL;
    ctx->regs.x64.rsi = 0xffffffffffffffffULL;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&byte_value;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)&word_value;
    ctx->regs.x64.rbp = (uint64_t)(uintptr_t)&dword_value;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x11223344556677abULL);
    ASSERT(ctx->regs.x64.rdx == 0x887766554433cdefULL);
    ASSERT(ctx->regs.x64.rsi == 0x12345678ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_scalar_memory_segment_family) {
    uint8_t code[] = {
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, /* movq %gs:0x58, %rax */
        0x65, 0x89, 0x0c, 0x25, 0x60, 0x00, 0x00, 0x00        /* movl %ecx, %gs:0x60 */
    };
    uint8_t teb_area[0x80] = {0};
    uint64_t loaded = 0x0123456789abcdefULL;
    uint32_t stored = 0;
    uint64_t base = (uint64_t)(uintptr_t)code;

    memcpy(teb_area + 0x58, &loaded, sizeof(loaded));
    memcpy(teb_area + 0x60, &stored, sizeof(stored));

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)teb_area, sizeof(teb_area),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);

    ctx->pc = base;
    ctx->gs_base = (uint64_t)(uintptr_t)teb_area;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = 0xaabbccddULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == loaded);
    memcpy(&stored, teb_area + 0x60, sizeof(stored));
    ASSERT(stored == 0xaabbccddu);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_direct_user_memory_fast_path_family) {
    uint8_t code[] = {
        0x43, 0x8a, 0x04, 0x08, /* movb (%r8,%r9), %al */
        0x41, 0x88, 0x01        /* movb %al, (%r9) */
    };
    uint8_t src[4] = {0x10, 0x20, 0x7b, 0x40};
    uint8_t dst[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);

    ctx->pc = base;
    ctx->regs.x64.rax = 0x1122334455667700ULL;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)&dst[1];
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)&src[2] - ctx->regs.x64.r9;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x112233445566777bULL);
    ASSERT(dst[1] == 0x7b);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_hot_byte_scan_loop_native) {
    uint8_t text[] = {'a', 'b', 'c', 0};
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x1000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* cmp = hb_ir_emit_cmp(b, hb_ir_mem(HB_REG_RBX, HB_REG_RAX, 1, 0, HB_SIZE_8),
                                        hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x1000);
    ASSERT(add && cmp && jcc);
    add->guest_addr = 0x1000; add->guest_len = 3;
    cmp->guest_addr = 0x1003; cmp->guest_len = 4;
    jcc->guest_addr = 0x1007; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)text, sizeof(text), HB_PERM_READ) == HB_OK);
    ctx->pc = 0x1000;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)text;
    ctx->regs.x64.rax = UINT64_MAX;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    bool equal = false;
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rax == 3);
    ASSERT(ctx->pc == 0x1009);
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    char* saved_codegen = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved_codegen);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_add8_partial_flags) {
    hb_ir_func_t* func = hb_ir_func_create(0x3000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_imm(1, HB_SIZE_8));
    ASSERT(add != NULL);
    add->guest_addr = 0x3000; add->guest_len = 3;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3000;
    ctx->regs.x64.rax = 0x11223344556677ffULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rax == 0x1122334455667700ULL);
    bool set = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &set) == HB_OK);
    ASSERT(set);
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_B, &set) == HB_OK);
    ASSERT(set);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_sub64_flags) {
    hb_ir_func_t* func = hb_ir_func_create(0x3100, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3100);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    ASSERT(sub != NULL);
    sub->guest_addr = 0x3100; sub->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3100;
    ctx->regs.x64.rdx = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rdx == UINT64_MAX);
    bool set = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_B, &set) == HB_OK);
    ASSERT(set);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_test32_zero_flags) {
    hb_ir_func_t* func = hb_ir_func_create(0x3200, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3200);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                          hb_ir_imm(0xff, HB_SIZE_32));
    ASSERT(test != NULL);
    test->guest_addr = 0x3200; test->guest_len = 5;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3200;
    ctx->regs.x64.rax = 0x100;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    bool set = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &set) == HB_OK);
    ASSERT(set);
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_B, &set) == HB_OK);
    ASSERT(!set);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_sub_jcc_pair_taken) {
    hb_ir_func_t* func = hb_ir_func_create(0x3300, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3300);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3400);
    ASSERT(sub && jcc);
    sub->guest_addr = 0x3300; sub->guest_len = 4;
    jcc->guest_addr = 0x3304; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3300;
    ctx->regs.x64.rdx = 2;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rdx == 1);
    ASSERT(ctx->pc == 0x3400);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    char* saved_codegen = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved_codegen);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_test_jcc_pair_taken) {
    hb_ir_func_t* func = hb_ir_func_create(0x3500, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3500);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                          hb_ir_imm(0xff, HB_SIZE_32));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3600);
    ASSERT(test && jcc);
    test->guest_addr = 0x3500; test->guest_len = 5;
    jcc->guest_addr = 0x3505; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x3500;
    ctx->regs.x64.rax = 0x100;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x3600);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    ASSERT(code_buf->size <= 260);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_copy_scan_body_fallthrough) {
    uint8_t src[2] = {'z', 0};
    uint8_t dst[2] = {0xaa, 0xbb};
    hb_ir_func_t* func = hb_ir_func_create(0x3700, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3700);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_mem(HB_REG_R8, HB_REG_R9, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3714);
    ASSERT(load && store && add && test && jcc);
    load->guest_addr = 0x3700; load->guest_len = 4;
    store->guest_addr = 0x3704; store->guest_len = 3;
    add->guest_addr = 0x3707; add->guest_len = 3;
    test->guest_addr = 0x370a; test->guest_len = 2;
    jcc->guest_addr = 0x370c; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3700;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src - ctx->regs.x64.r9;
    ctx->regs.x64.rax = 0x1122334455667700ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(dst[0] == 'z');
    ASSERT(ctx->regs.x64.rax == 0x112233445566777aULL);
    ASSERT(ctx->regs.x64.r9 == (uint64_t)(uintptr_t)(dst + 1));
    ASSERT(ctx->pc == 0x370e);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 260);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_copy_scan_body_taken) {
    uint8_t src[1] = {0};
    uint8_t dst[1] = {0xaa};
    hb_ir_func_t* func = hb_ir_func_create(0x3800, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3800);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_mem(HB_REG_R8, HB_REG_R9, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3814);
    ASSERT(load && store && add && test && jcc);
    load->guest_addr = 0x3800; load->guest_len = 4;
    store->guest_addr = 0x3804; store->guest_len = 3;
    add->guest_addr = 0x3807; add->guest_len = 3;
    test->guest_addr = 0x380a; test->guest_len = 2;
    jcc->guest_addr = 0x380c; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3800;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src - ctx->regs.x64.r9;
    ctx->regs.x64.rax = 0x112233445566777fULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(dst[0] == 0);
    ASSERT(ctx->regs.x64.rax == 0x1122334455667700ULL);
    ASSERT(ctx->regs.x64.r9 == (uint64_t)(uintptr_t)(dst + 1));
    ASSERT(ctx->pc == 0x3814);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_copy_scan_counted_loop_zero_exit) {
    uint8_t src[3] = {'a', 'b', 0};
    uint8_t dst[3] = {0xaa, 0xbb, 0xcc};
    hb_ir_func_t* func = hb_ir_func_create(0x3900, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* body = hb_ir_block_create(0, 0x3900);
    hb_ir_block_t* guard = hb_ir_block_create(1, 0x390e);
    ASSERT(body && guard);
    hb_ir_cfg_add_block(func->cfg, body);
    hb_ir_cfg_add_block(func->cfg, guard);
    func->cfg->entry = body;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, body);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_mem(HB_REG_R8, HB_REG_R9, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* body_jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3920);
    ASSERT(load && store && add && test && body_jcc);
    load->guest_addr = 0x3900; load->guest_len = 4;
    store->guest_addr = 0x3904; store->guest_len = 3;
    add->guest_addr = 0x3907; add->guest_len = 3;
    test->guest_addr = 0x390a; test->guest_len = 2;
    body_jcc->guest_addr = 0x390c; body_jcc->guest_len = 2;

    hb_ir_builder_set_block(b, guard);
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* guard_jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3900);
    ASSERT(sub && guard_jcc);
    sub->guest_addr = 0x390e; sub->guest_len = 4;
    guard_jcc->guest_addr = 0x3912; guard_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3900;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src - ctx->regs.x64.r9;
    ctx->regs.x64.rdx = 10;
    ctx->regs.x64.rax = 0x112233445566777fULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    ASSERT(dst[0] == 'a' && dst[1] == 'b' && dst[2] == 0);
    ASSERT(ctx->regs.x64.r9 == (uint64_t)(uintptr_t)(dst + 3));
    ASSERT(ctx->regs.x64.rdx == 8);
    ASSERT(ctx->regs.x64.rax == 0x1122334455667700ULL);
    ASSERT(ctx->pc == 0x3920);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(768);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    ASSERT(hb_arm64_codegen_block_with_cfg(cg, body, func->cfg, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 480);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_copy_scan_counted_loop_count_exit) {
    uint8_t src[3] = {'a', 'b', 'c'};
    uint8_t dst[3] = {0xaa, 0xbb, 0xcc};
    hb_ir_func_t* func = hb_ir_func_create(0x3a00, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* body = hb_ir_block_create(0, 0x3a00);
    hb_ir_block_t* guard = hb_ir_block_create(1, 0x3a0e);
    ASSERT(body && guard);
    hb_ir_cfg_add_block(func->cfg, body);
    hb_ir_cfg_add_block(func->cfg, guard);
    func->cfg->entry = body;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, body);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_mem(HB_REG_R8, HB_REG_R9, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* body_jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3a20);
    ASSERT(load && store && add && test && body_jcc);
    load->guest_addr = 0x3a00; load->guest_len = 4;
    store->guest_addr = 0x3a04; store->guest_len = 3;
    add->guest_addr = 0x3a07; add->guest_len = 3;
    test->guest_addr = 0x3a0a; test->guest_len = 2;
    body_jcc->guest_addr = 0x3a0c; body_jcc->guest_len = 2;

    hb_ir_builder_set_block(b, guard);
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* guard_jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3a00);
    ASSERT(sub && guard_jcc);
    sub->guest_addr = 0x3a0e; sub->guest_len = 4;
    guard_jcc->guest_addr = 0x3a12; guard_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3a00;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src - ctx->regs.x64.r9;
    ctx->regs.x64.rdx = 2;
    ctx->regs.x64.rax = 0x1122334455667700ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    ASSERT(dst[0] == 'a' && dst[1] == 'b' && dst[2] == 0xcc);
    ASSERT(ctx->regs.x64.r9 == (uint64_t)(uintptr_t)(dst + 2));
    ASSERT(ctx->regs.x64.rdx == 0);
    ASSERT(ctx->regs.x64.rax == 0x1122334455667762ULL);
    ASSERT(ctx->pc == 0x3a14);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_copy_scan_counted_loop_promotes_cache_pair) {
    uint8_t src[3] = {'a', 'b', 0};
    uint8_t dst[3] = {0xaa, 0xbb, 0xcc};
    hb_ir_func_t* body_func = hb_ir_func_create(0x3b00, 0);
    hb_ir_func_t* guard_func = hb_ir_func_create(0x3b0e, 0);
    ASSERT(body_func && guard_func);
    hb_ir_block_t* body = hb_ir_block_create(0, 0x3b00);
    hb_ir_block_t* guard = hb_ir_block_create(0, 0x3b0e);
    ASSERT(body && guard);
    hb_ir_cfg_add_block(body_func->cfg, body);
    hb_ir_cfg_add_block(guard_func->cfg, guard);
    body_func->cfg->entry = body;
    guard_func->cfg->entry = guard;

    hb_ir_builder_t* b = hb_ir_builder_create(body_func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, body);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_mem(HB_REG_R8, HB_REG_R9, 1, 0, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_R9, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_8),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* body_jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3b20);
    ASSERT(load && store && add && test && body_jcc);
    load->guest_addr = 0x3b00; load->guest_len = 4;
    store->guest_addr = 0x3b04; store->guest_len = 3;
    add->guest_addr = 0x3b07; add->guest_len = 3;
    test->guest_addr = 0x3b0a; test->guest_len = 2;
    body_jcc->guest_addr = 0x3b0c; body_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    b = hb_ir_builder_create(guard_func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, guard);
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* guard_jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3b00);
    ASSERT(sub && guard_jcc);
    sub->guest_addr = 0x3b0e; sub->guest_len = 4;
    guard_jcc->guest_addr = 0x3b12; guard_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src), HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3b00;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src - ctx->regs.x64.r9;
    ctx->regs.x64.rdx = 10;
    ctx->regs.x64.rax = 0x112233445566777fULL;

    hb_jit_runtime_t* rt = hb_jit_runtime_create(ctx);
    ASSERT(rt != NULL);
    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);

    hb_exec_result_t out;
    ASSERT(hb_jit_runtime_run(rt, body_func, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(dst[0] == 'a');
    ASSERT(ctx->pc == 0x3b0e);

    ASSERT(hb_jit_runtime_run(rt, guard_func, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == 0x3b00);
    ASSERT(ctx->regs.x64.rdx == 9);

    ASSERT(hb_jit_runtime_run(rt, body_func, &out) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(dst[0] == 'a' && dst[1] == 'b' && dst[2] == 0);
    ASSERT(ctx->regs.x64.r9 == (uint64_t)(uintptr_t)(dst + 3));
    ASSERT(ctx->regs.x64.rdx == 8);
    ASSERT(ctx->regs.x64.rax == 0x1122334455667700ULL);
    ASSERT(ctx->pc == 0x3b20);

    hb_jit_runtime_destroy(rt);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(body_func);
    hb_ir_func_destroy(guard_func);
    tests_passed++;
}

TEST(jit_x64_native_bounded_scan_loop_promotes_cache_pair) {
    uint8_t text[3] = {'a', 'b', 0};
    hb_ir_func_t* guard_func = hb_ir_func_create(0x3c00, 0);
    hb_ir_func_t* body_func = hb_ir_func_create(0x3c05, 0);
    ASSERT(guard_func && body_func);
    hb_ir_block_t* guard = hb_ir_block_create(0, 0x3c00);
    hb_ir_block_t* body = hb_ir_block_create(0, 0x3c05);
    ASSERT(guard && body);
    hb_ir_cfg_add_block(guard_func->cfg, guard);
    hb_ir_cfg_add_block(body_func->cfg, body);
    guard_func->cfg->entry = guard;
    body_func->cfg->entry = body;

    hb_ir_builder_t* b = hb_ir_builder_create(guard_func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, guard);
    hb_ir_instr_t* cmp_guard = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                              hb_ir_reg(HB_REG_RDX, HB_SIZE_64));
    hb_ir_instr_t* guard_jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x3c0e);
    ASSERT(cmp_guard && guard_jcc);
    cmp_guard->guest_addr = 0x3c00; cmp_guard->guest_len = 3;
    guard_jcc->guest_addr = 0x3c03; guard_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    b = hb_ir_builder_create(body_func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, body);
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* cmp_body = hb_ir_emit_cmp(b, hb_ir_mem(HB_REG_RAX, HB_REG_RCX, 1, 0, HB_SIZE_8),
                                             hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* body_jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3c00);
    ASSERT(add && cmp_body && body_jcc);
    add->guest_addr = 0x3c05; add->guest_len = 3;
    cmp_body->guest_addr = 0x3c08; cmp_body->guest_len = 4;
    body_jcc->guest_addr = 0x3c0c; body_jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)text, sizeof(text), HB_PERM_READ) == HB_OK);
    ctx->pc = 0x3c00;
    ctx->regs.x64.rax = UINT64_MAX;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)text;
    ctx->regs.x64.rdx = 10;

    hb_jit_runtime_t* rt = hb_jit_runtime_create(ctx);
    ASSERT(rt != NULL);
    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);

    hb_exec_result_t out;
    ASSERT(hb_jit_runtime_run(rt, guard_func, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == 0x3c05);

    ASSERT(hb_jit_runtime_run(rt, body_func, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->pc == 0x3c00);
    ASSERT(ctx->regs.x64.rax == 0);

    ASSERT(hb_jit_runtime_run(rt, guard_func, &out) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rax == 2);
    ASSERT(ctx->pc == 0x3c0e);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_jit_runtime_destroy(rt);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(guard_func);
    hb_ir_func_destroy(body_func);
    tests_passed++;
}

TEST(jit_x64_native_store_count_loop_imm_limit) {
    uint8_t dst[6] = {0};
    hb_ir_func_t* func = hb_ir_func_create(0x3d00, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3d00);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RSI, HB_SIZE_8));
    hb_ir_instr_t* inc_count = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                                hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                                hb_ir_imm(1, HB_SIZE_32));
    hb_ir_instr_t* inc_ptr = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                                              hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                                              hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* cmp = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                        hb_ir_imm(5, HB_SIZE_32));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_B, 0x3d00);
    ASSERT(store && inc_count && inc_ptr && cmp && jcc);
    store->guest_addr = 0x3d00; store->guest_len = 3;
    inc_count->guest_addr = 0x3d03; inc_count->guest_len = 2;
    inc_ptr->guest_addr = 0x3d05; inc_ptr->guest_len = 3;
    cmp->guest_addr = 0x3d08; cmp->guest_len = 3;
    jcc->guest_addr = 0x3d0b; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3d00;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.rsi = 'x';

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(memcmp(dst, "xxxxx", 5) == 0);
    ASSERT(ctx->regs.x64.rax == 5);
    ASSERT(ctx->regs.x64.rcx == (uint64_t)(uintptr_t)(dst + 5));
    ASSERT(ctx->pc == 0x3d0d);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_store_count_loop_reg_limit_src_count) {
    uint8_t dst[4] = {0xff, 0xff, 0xff, 0xff};
    hb_ir_func_t* func = hb_ir_func_create(0x3e00, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3e00);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RAX, HB_SIZE_8));
    hb_ir_instr_t* inc_count = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                                hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                                hb_ir_imm(1, HB_SIZE_32));
    hb_ir_instr_t* inc_ptr = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                                              hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                                              hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* cmp = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_32),
                                        hb_ir_reg(HB_REG_RSI, HB_SIZE_32));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_B, 0x3e00);
    ASSERT(store && inc_count && inc_ptr && cmp && jcc);
    store->guest_addr = 0x3e00; store->guest_len = 2;
    inc_count->guest_addr = 0x3e02; inc_count->guest_len = 2;
    inc_ptr->guest_addr = 0x3e04; inc_ptr->guest_len = 3;
    cmp->guest_addr = 0x3e07; cmp->guest_len = 2;
    jcc->guest_addr = 0x3e09; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst), HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3e00;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.rsi = 3;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(dst[0] == 0 && dst[1] == 1 && dst[2] == 2 && dst[3] == 0xff);
    ASSERT(ctx->regs.x64.rax == 3);
    ASSERT(ctx->regs.x64.rcx == (uint64_t)(uintptr_t)(dst + 3));
    ASSERT(ctx->pc == 0x3e0b);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_zero_store_update_backedge) {
    uint8_t bytes[8] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22};
    hb_ir_func_t* func = hb_ir_func_create(0x3f00, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3f00);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* zero = hb_ir_emit_binop(b, HB_IR_XOR, hb_ir_reg(HB_REG_RCX, HB_SIZE_8),
                                           hb_ir_reg(HB_REG_RCX, HB_SIZE_8),
                                           hb_ir_reg(HB_REG_RCX, HB_SIZE_8));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RBX, HB_REG_RAX, 1, 0x118, HB_SIZE_8),
                                            hb_ir_reg(HB_REG_RCX, HB_SIZE_8));
    hb_ir_instr_t* add_ptr2 = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                               hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                               hb_ir_imm(2, HB_SIZE_64));
    hb_ir_instr_t* add_index = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                                hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                                hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* sub_count = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                                hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                                hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x3f00);
    ASSERT(zero && store && add_ptr2 && add_index && sub_count && jcc);
    zero->guest_addr = 0x3f00; zero->guest_len = 2;
    store->guest_addr = 0x3f02; store->guest_len = 7;
    add_ptr2->guest_addr = 0x3f09; add_ptr2->guest_len = 4;
    add_index->guest_addr = 0x3f0d; add_index->guest_len = 3;
    sub_count->guest_addr = 0x3f10; sub_count->guest_len = 4;
    jcc->guest_addr = 0x3f14; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)bytes, sizeof(bytes),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3f00;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rbx = (uint64_t)((uintptr_t)bytes - 0x118u);
    ctx->regs.x64.rcx = 0x1122334455667788ULL;
    ctx->regs.x64.rdx = 0x40;
    ctx->regs.x64.rsi = 2;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 2);
    ASSERT(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xcc);
    ASSERT(ctx->regs.x64.rax == 2);
    ASSERT(ctx->regs.x64.rdx == 0x44);
    ASSERT(ctx->regs.x64.rsi == 0);
    ASSERT(ctx->regs.x64.rcx == 0x1122334455667700ULL);
    ASSERT(ctx->pc == 0x3f16);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 320);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_logic_rmw_memory_family) {
    uint8_t bytes[64] = {0};
    uint16_t word = 0xf0f0;
    uint32_t dword = 0x00ff00ff;
    bytes[0x1a] = 0x01;

    hb_ir_func_t* func = hb_ir_func_create(0x3f00, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x3f00);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_operand_t or_mem = hb_ir_mem(HB_REG_RBX, HB_REG_RAX, 1, 0x18, HB_SIZE_8);
    hb_ir_operand_t and_mem = hb_ir_mem(HB_REG_RDI, HB_REG_COUNT, 1, 0, HB_SIZE_16);
    hb_ir_operand_t xor_mem = hb_ir_mem(HB_REG_RSI, HB_REG_COUNT, 1, 0, HB_SIZE_32);
    hb_ir_instr_t* or_i = hb_ir_emit_binop(b, HB_IR_OR, or_mem, or_mem,
                                           hb_ir_imm(0x10, HB_SIZE_8));
    hb_ir_instr_t* and_i = hb_ir_emit_binop(b, HB_IR_AND, and_mem, and_mem,
                                            hb_ir_reg(HB_REG_RDX, HB_SIZE_16));
    hb_ir_instr_t* xor_i = hb_ir_emit_binop(b, HB_IR_XOR, xor_mem, xor_mem,
                                            hb_ir_imm(0x00ff00ff, HB_SIZE_32));
    ASSERT(or_i && and_i && xor_i);
    or_i->guest_addr = 0x3f00; or_i->guest_len = 5;
    and_i->guest_addr = 0x3f05; and_i->guest_len = 3;
    xor_i->guest_addr = 0x3f08; xor_i->guest_len = 6;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)bytes, sizeof(bytes),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&word, sizeof(word),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&dword, sizeof(dword),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x3f00;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)bytes;
    ctx->regs.x64.rax = 2;
    ctx->regs.x64.rdi = (uint64_t)(uintptr_t)&word;
    ctx->regs.x64.rsi = (uint64_t)(uintptr_t)&dword;
    ctx->regs.x64.rdx = 0x00ff;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(bytes[0x1a] == 0x11);
    ASSERT(word == 0x00f0);
    ASSERT(dword == 0);
    ASSERT(ctx->pc == 0x3f0e);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_mem_imm_test_jcc_pair) {
    uint8_t byte = 0x02;
    hb_ir_func_t* func = hb_ir_func_create(0x4000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_mem(HB_REG_RDX, HB_REG_COUNT, 1, 0, HB_SIZE_8),
                                          hb_ir_imm(0x01, HB_SIZE_8));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x4010);
    ASSERT(test && jcc);
    test->guest_addr = 0x4000; test->guest_len = 3;
    jcc->guest_addr = 0x4003; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&byte, sizeof(byte),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4000;
    ctx->regs.x64.rdx = (uint64_t)(uintptr_t)&byte;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x4010);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 192);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_mem_imm_test_jcc_pair_word_fallthrough) {
    uint16_t word = 0x0002;
    hb_ir_func_t* func = hb_ir_func_create(0x4200, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4200);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_mem(HB_REG_RDX, HB_REG_COUNT, 1, 0, HB_SIZE_16),
                                          hb_ir_imm(0x0004, HB_SIZE_16));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x4210);
    ASSERT(test && jcc);
    test->guest_addr = 0x4200; test->guest_len = 4;
    jcc->guest_addr = 0x4204; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&word, sizeof(word),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4200;
    ctx->regs.x64.rdx = (uint64_t)(uintptr_t)&word;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x4206);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 188);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_mem_imm_cmp_jcc_pair) {
    uint32_t value = 7;
    hb_ir_func_t* func = hb_ir_func_create(0x4100, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x4100);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* cmp = hb_ir_emit_cmp(b, hb_ir_mem(HB_REG_RBX, HB_REG_COUNT, 1, 0, HB_SIZE_32),
                                        hb_ir_imm(0, HB_SIZE_32));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x4110);
    ASSERT(cmp && jcc);
    cmp->guest_addr = 0x4100; cmp->guest_len = 7;
    jcc->guest_addr = 0x4107; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&value, sizeof(value),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x4100;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&value;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x4110);
    bool equal = true;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(!equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 252);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_zero_test_jcc_block_family) {
    for (int use_byte = 0; use_byte <= 1; use_byte++) {
        hb_size_t size = use_byte ? HB_SIZE_8 : HB_SIZE_32;
        hb_cc_t cc = use_byte ? HB_CC_NE : HB_CC_E;
        uint64_t target = use_byte ? 0x5010 : 0x5018;
        uint64_t fallthrough = 0x5006;
        hb_ir_func_t* func = hb_ir_func_create(0x5000, 0);
        ASSERT(func != NULL);
        hb_ir_block_t* blk = hb_ir_block_create(0, 0x5000);
        ASSERT(blk != NULL);
        hb_ir_cfg_add_block(func->cfg, blk);
        func->cfg->entry = blk;

        hb_ir_builder_t* b = hb_ir_builder_create(func);
        ASSERT(b != NULL);
        hb_ir_builder_set_block(b, blk);
        hb_ir_operand_t reg = hb_ir_reg(HB_REG_RAX, size);
        hb_ir_instr_t* xor_i = hb_ir_emit_binop(b, HB_IR_XOR, reg, reg, reg);
        hb_ir_instr_t* test_i = hb_ir_emit_test(b, reg, reg);
        hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, cc, target);
        ASSERT(xor_i && test_i && jcc);
        xor_i->guest_addr = 0x5000; xor_i->guest_len = 2;
        test_i->guest_addr = 0x5002; test_i->guest_len = 2;
        jcc->guest_addr = 0x5004; jcc->guest_len = 2;
        hb_ir_builder_destroy(b);

        hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
        ASSERT(ctx != NULL);
        ctx->pc = 0x5000;
        ctx->regs.x64.rax = use_byte ? 0xdeadbeefcafeba77ULL : UINT64_MAX;
        hb_exec_result_t out;
        ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
        ASSERT(out.result == HB_OK);
        ASSERT_EQ(out.blocks_executed, 1);
        ASSERT(ctx->pc == (use_byte ? fallthrough : target));
        ASSERT(ctx->regs.x64.rax == (use_byte ? 0xdeadbeefcafeba00ULL : 0));
        bool equal = false;
        ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
        ASSERT(equal);

        hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
        hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
        ASSERT(code_buf != NULL && cg != NULL);
        ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
        ASSERT(code_buf->size <= 180);
        hb_arm64_codegen_destroy(cg);
        hb_codegen_buffer_destroy(code_buf);

        hb_context_destroy(ctx);
        hb_ir_func_destroy(func);
    }
    tests_passed++;
}

TEST(jit_x64_native_adjacent_mem64_pair_family) {
    uint64_t slots[4] = {0};
    hb_ir_func_t* func = hb_ir_func_create(0x5100, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5100);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store1 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
    hb_ir_instr_t* store2 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RSI, HB_SIZE_64));
    hb_ir_instr_t* load1 = hb_ir_emit_load(b, hb_ir_reg(HB_REG_R10, HB_SIZE_64),
                                           hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    hb_ir_instr_t* load2 = hb_ir_emit_load(b, hb_ir_reg(HB_REG_R11, HB_SIZE_64),
                                           hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64));
    ASSERT(store1 && store2 && load1 && load2);
    store1->guest_addr = 0x5100; store1->guest_len = 5;
    store2->guest_addr = 0x5105; store2->guest_len = 5;
    load1->guest_addr = 0x510a; load1->guest_len = 5;
    load2->guest_addr = 0x510f; load2->guest_len = 5;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)slots, sizeof(slots),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5100;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)slots;
    ctx->regs.x64.rbx = 0x1122334455667788ULL;
    ctx->regs.x64.rsi = 0x99aabbccddeeff00ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(slots[1] == 0x1122334455667788ULL);
    ASSERT(slots[2] == 0x99aabbccddeeff00ULL);
    ASSERT(ctx->regs.x64.r10 == slots[1]);
    ASSERT(ctx->regs.x64.r11 == slots[2]);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 132);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_stack_spill_push_sub_prologue) {
    uint64_t slots[10] = {0};
    hb_ir_func_t* func = hb_ir_func_create(0x5200, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5200);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store1 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
    hb_ir_instr_t* store2 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RSI, HB_SIZE_64));
    hb_ir_instr_t* push = hb_ir_emit_push(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64));
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                          hb_ir_imm(0x20, HB_SIZE_64));
    ASSERT(store1 && store2 && push && sub);
    store1->guest_addr = 0x5200; store1->guest_len = 5;
    store2->guest_addr = 0x5205; store2->guest_len = 5;
    push->guest_addr = 0x520a; push->guest_len = 1;
    sub->guest_addr = 0x520b; sub->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)slots, sizeof(slots),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5200;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)&slots[5];
    ctx->regs.x64.rbx = 0x1111222233334444ULL;
    ctx->regs.x64.rsi = 0x5555666677778888ULL;
    ctx->regs.x64.rdi = 0x9999aaaabbbbccccULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)&slots[0]);
    ASSERT(slots[4] == 0x9999aaaabbbbccccULL);
    ASSERT(slots[6] == 0x1111222233334444ULL);
    ASSERT(slots[7] == 0x5555666677778888ULL);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 164);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_prologue_local_init_test_block) {
    uint64_t slots[10] = {0};
    uint8_t object[64];
    memset(object, 0xcc, sizeof(object));

    hb_ir_func_t* func = hb_ir_func_create(0x5240, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5240);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store1 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 8, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RBX, HB_SIZE_64));
    hb_ir_instr_t* store2 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RSP, HB_REG_COUNT, 1, 16, HB_SIZE_64),
                                             hb_ir_reg(HB_REG_RSI, HB_SIZE_64));
    hb_ir_instr_t* push = hb_ir_emit_push(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64));
    hb_ir_instr_t* sub = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RSP, HB_SIZE_64),
                                          hb_ir_imm(0x20, HB_SIZE_64));
    hb_ir_instr_t* store_zero = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 0x18, HB_SIZE_8),
                                                 hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* mov = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64),
                                        hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
    hb_ir_instr_t* lea = hb_ir_emit_lea(b, hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                        hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x5270);
    ASSERT(store1 && store2 && push && sub && store_zero && mov && lea && test && jcc);
    store1->guest_addr = 0x5240; store1->guest_len = 5;
    store2->guest_addr = 0x5245; store2->guest_len = 5;
    push->guest_addr = 0x524a; push->guest_len = 1;
    sub->guest_addr = 0x524b; sub->guest_len = 4;
    store_zero->guest_addr = 0x524f; store_zero->guest_len = 4;
    mov->guest_addr = 0x5253; mov->guest_len = 3;
    lea->guest_addr = 0x5256; lea->guest_len = 4;
    test->guest_addr = 0x525a; test->guest_len = 3;
    jcc->guest_addr = 0x525d; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)slots, sizeof(slots),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)object, sizeof(object),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5240;
    ctx->regs.x64.rsp = (uint64_t)(uintptr_t)&slots[5];
    ctx->regs.x64.rbx = 0x1111222233334444ULL;
    ctx->regs.x64.rsi = 0x5555666677778888ULL;
    ctx->regs.x64.rdi = 0x9999aaaabbbbccccULL;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)object;
    ctx->regs.x64.rdx = 0;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rsp == (uint64_t)(uintptr_t)&slots[0]);
    ASSERT(slots[4] == 0x9999aaaabbbbccccULL);
    ASSERT(slots[6] == 0x1111222233334444ULL);
    ASSERT(slots[7] == 0x5555666677778888ULL);
    ASSERT(object[0x18] == 0);
    ASSERT(ctx->regs.x64.rdi == (uint64_t)(uintptr_t)object);
    ASSERT(ctx->regs.x64.rsi == (uint64_t)(uintptr_t)(object + 8));
    ASSERT(ctx->pc == 0x5270);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_mov_lea_same_base_pair) {
    hb_ir_func_t* func = hb_ir_func_create(0x5300, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5300);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* mov = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64),
                                        hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
    hb_ir_instr_t* lea = hb_ir_emit_lea(b, hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                        hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    ASSERT(mov && lea);
    mov->guest_addr = 0x5300; mov->guest_len = 3;
    lea->guest_addr = 0x5303; lea->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x5300;
    ctx->regs.x64.rcx = 0x1122334455667700ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rdi == 0x1122334455667700ULL);
    ASSERT(ctx->regs.x64.rsi == 0x1122334455667708ULL);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    ASSERT(code_buf->size <= 72);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_store_imm_mov_lea_same_base) {
    uint8_t mem[64];
    memset(mem, 0xcc, sizeof(mem));
    hb_ir_func_t* func = hb_ir_func_create(0x5320, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5320);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 0x18, HB_SIZE_8),
                                            hb_ir_imm(0, HB_SIZE_8));
    hb_ir_instr_t* mov = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RDI, HB_SIZE_64),
                                        hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
    hb_ir_instr_t* lea = hb_ir_emit_lea(b, hb_ir_reg(HB_REG_RSI, HB_SIZE_64),
                                        hb_ir_mem(HB_REG_RCX, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    ASSERT(store && mov && lea);
    store->guest_addr = 0x5320; store->guest_len = 4;
    mov->guest_addr = 0x5324; mov->guest_len = 3;
    lea->guest_addr = 0x5327; lea->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mem, sizeof(mem),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5320;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)mem;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(mem[0x18] == 0);
    ASSERT(ctx->regs.x64.rdi == (uint64_t)(uintptr_t)mem);
    ASSERT(ctx->regs.x64.rsi == (uint64_t)(uintptr_t)(mem + 8));

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 80);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_xmm_load_store_pair) {
    uint64_t src[2] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    uint64_t dst[2] = {0, 0};
    hb_ir_func_t* func = hb_ir_func_create(0x5360, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5360);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load = hb_ir_emit_load(b, hb_ir_reg(HB_REG_XMM0, HB_SIZE_128),
                                          hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 0, HB_SIZE_128));
    hb_ir_instr_t* store = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 0, HB_SIZE_128),
                                            hb_ir_reg(HB_REG_XMM0, HB_SIZE_128));
    ASSERT(load && store);
    load->guest_addr = 0x5360; load->guest_len = 5;
    store->guest_addr = 0x5365; store->guest_len = 5;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5360;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.xmm[0][0] == src[0] && ctx->regs.x64.xmm[0][1] == src[1]);
    ASSERT(dst[0] == src[0] && dst[1] == src[1]);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 96);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_scalar_load_store_pair) {
    uint8_t src[32];
    uint8_t dst[32];
    uint64_t copied64;
    memset(src, 0, sizeof(src));
    memset(dst, 0xcc, sizeof(dst));
    copied64 = 0x1122334455667788ULL;
    memcpy(src + 8, &copied64, sizeof(copied64));
    src[1] = 0x5a;

    hb_ir_func_t* func = hb_ir_func_create(0x5380, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5380);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* load64 = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RCX, HB_SIZE_64),
                                            hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 8, HB_SIZE_64));
    hb_ir_instr_t* store64 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 16, HB_SIZE_64),
                                              hb_ir_reg(HB_REG_RCX, HB_SIZE_64));
    hb_ir_instr_t* load8 = hb_ir_emit_load(b, hb_ir_reg(HB_REG_RDX, HB_SIZE_8),
                                           hb_ir_mem(HB_REG_R8, HB_REG_COUNT, 1, 1, HB_SIZE_8));
    hb_ir_instr_t* store8 = hb_ir_emit_store(b, hb_ir_mem(HB_REG_R9, HB_REG_COUNT, 1, 3, HB_SIZE_8),
                                             hb_ir_reg(HB_REG_RDX, HB_SIZE_8));
    ASSERT(load64 && store64 && load8 && store8);
    load64->guest_addr = 0x5380; load64->guest_len = 5;
    store64->guest_addr = 0x5385; store64->guest_len = 5;
    load8->guest_addr = 0x538a; load8->guest_len = 4;
    store8->guest_addr = 0x538e; store8->guest_len = 4;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = 0x5380;
    ctx->regs.x64.r8 = (uint64_t)(uintptr_t)src;
    ctx->regs.x64.r9 = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.rdx = 0xaabbccddeeff0011ULL;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rcx == 0x1122334455667788ULL);
    ASSERT(ctx->regs.x64.rdx == 0xaabbccddeeff005aULL);
    copied64 = 0;
    memcpy(&copied64, dst + 16, sizeof(copied64));
    ASSERT(copied64 == 0x1122334455667788ULL);
    ASSERT(dst[3] == 0x5a);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);
    ASSERT(code_buf->size <= 144);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_native_test_same_reg_jcc_pair) {
    hb_ir_func_t* func = hb_ir_func_create(0x5340, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x5340);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* test = hb_ir_emit_test(b, hb_ir_reg(HB_REG_RDX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RDX, HB_SIZE_64));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_E, 0x5350);
    ASSERT(test && jcc);
    test->guest_addr = 0x5340; test->guest_len = 3;
    jcc->guest_addr = 0x5343; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->pc = 0x5340;
    ctx->regs.x64.rdx = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->pc == 0x5350);
    bool equal = false;
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    ASSERT(code_buf->size <= 160);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_hot_word_scan_loop_native) {
    uint16_t text[] = {'o', 'k', 0};
    hb_ir_func_t* func = hb_ir_func_create(0x2000, 0);
    ASSERT(func != NULL);
    hb_ir_block_t* blk = hb_ir_block_create(0, 0x2000);
    ASSERT(blk != NULL);
    hb_ir_cfg_add_block(func->cfg, blk);
    func->cfg->entry = blk;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    ASSERT(b != NULL);
    hb_ir_builder_set_block(b, blk);
    hb_ir_instr_t* add = hb_ir_emit_binop(b, HB_IR_ADD, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                          hb_ir_imm(1, HB_SIZE_64));
    hb_ir_instr_t* cmp = hb_ir_emit_cmp(b, hb_ir_mem(HB_REG_RSI, HB_REG_RAX, 2, 0, HB_SIZE_16),
                                        hb_ir_reg(HB_REG_R14, HB_SIZE_16));
    hb_ir_instr_t* jcc = hb_ir_emit_jcc(b, HB_CC_NE, 0x2000);
    ASSERT(add && cmp && jcc);
    add->guest_addr = 0x2000; add->guest_len = 3;
    cmp->guest_addr = 0x2003; cmp->guest_len = 5;
    jcc->guest_addr = 0x2008; jcc->guest_len = 2;
    hb_ir_builder_destroy(b);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)text, sizeof(text), HB_PERM_READ) == HB_OK);
    ctx->pc = 0x2000;
    ctx->regs.x64.rsi = (uint64_t)(uintptr_t)text;
    ctx->regs.x64.r14 = 0;
    ctx->regs.x64.rax = UINT64_MAX;

    char* saved = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    hb_exec_result_t out;
    hb_result_t r = hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved);

    bool equal = false;
    ASSERT(r == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(out.blocks_executed, 1);
    ASSERT(ctx->regs.x64.rax == 2);
    ASSERT(ctx->pc == 0x200a);
    ASSERT(hb_flags_eval_cond(ctx, HB_CC_E, &equal) == HB_OK);
    ASSERT(equal);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(512);
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    ASSERT(code_buf != NULL && cg != NULL);
    char* saved_codegen = save_env_var("MACRUNNER_HB_JIT_DIRECT_MEM");
    setenv("MACRUNNER_HB_JIT_DIRECT_MEM", "1", 1);
    ASSERT(hb_arm64_codegen_block(cg, blk, code_buf) == HB_OK);
    restore_env_var("MACRUNNER_HB_JIT_DIRECT_MEM", saved_codegen);
    ASSERT(code_buf->size <= 220);
    hb_arm64_codegen_destroy(cg);
    hb_codegen_buffer_destroy(code_buf);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_cmp_mem_operand_routes_to_helper) {
    uint8_t code[] = {
        0x48, 0x39, 0x0b,             /* cmpq %rcx, (%rbx) */
        0x75, 0x07,                   /* jne target */
        0xba, 0x01, 0x00, 0x00, 0x00, /* movl $1, %edx */
        0xeb, 0x05,                   /* jmp done */
        0xba, 0x02, 0x00, 0x00, 0x00, /* target: movl $2, %edx */
        0xc3                          /* done: ret */
    };
    uint64_t value = 0x1122334455667788ULL;
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&value, sizeof(value),
                         HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&value;
    ctx->regs.x64.rcx = value;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(ctx->pc, base + 5);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_mul_div_family_routes_to_helper) {
    uint8_t code[] = {
        0x48, 0xf7, 0xf3,       /* divq %rbx */
        0x49, 0x0f, 0xaf, 0xd8  /* imulq %r8, %rbx */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x100;
    ctx->regs.x64.rdx = 0;
    ctx->regs.x64.rbx = 0x10;
    ctx->regs.x64.r8 = 3;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT_EQ(ctx->regs.x64.rax, 0x10);
    ASSERT_EQ(ctx->regs.x64.rdx, 0);
    ASSERT_EQ(ctx->regs.x64.rbx, 0x30);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_interp_helper_covers_unity_mono_fallback_families) {
    uint8_t bt_code[] = {
        0x0f, 0xba, 0xe3, 0x09        /* btl $9, %ebx */
    };
    uint8_t xchg_code[] = {
        0x48, 0x87, 0x03              /* xchgq %rax, (%rbx) */
    };
    uint8_t xmm_code[] = {
        0x0f, 0x10, 0x03,             /* movups (%rbx), %xmm0 */
        0xf3, 0x0f, 0x7f, 0x01,       /* movdqu %xmm0, (%rcx) */
        0x66, 0x0f, 0x7f, 0x41, 0x10  /* movdqa %xmm0, 0x10(%rcx) */
    };
    uint8_t src[16] __attribute__((aligned(16))) = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
    };
    uint8_t dst[32] __attribute__((aligned(16))) = {0};
    uint64_t slot = 0x8877665544332211ULL;
    hb_exec_result_t out;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, bt_code, sizeof(bt_code),
                                          (uint64_t)(uintptr_t)bt_code);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)bt_code, sizeof(bt_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = (uint64_t)(uintptr_t)bt_code;
    ctx->regs.x64.rbx = 1ULL << 9;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.cf == true);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X64, xchg_code, sizeof(xchg_code),
                            (uint64_t)(uintptr_t)xchg_code);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)xchg_code, sizeof(xchg_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&slot, sizeof(slot),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = (uint64_t)(uintptr_t)xchg_code;
    ctx->regs.x64.rax = 0x1122334455667788ULL;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)&slot;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x8877665544332211ULL);
    ASSERT(slot == 0x1122334455667788ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    dec = hb_decoder_create(HB_ARCH_X64, xmm_code, sizeof(xmm_code),
                            (uint64_t)(uintptr_t)xmm_code);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)xmm_code, sizeof(xmm_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src),
                         HB_PERM_READ) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = (uint64_t)(uintptr_t)xmm_code;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)src;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)dst;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(memcmp(dst, src, sizeof(src)) == 0);
    ASSERT(memcmp(dst + 16, src, sizeof(src)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_bswap_family_notepadpp_message_path) {
    uint8_t code[] = {
        0x48, 0x0f, 0xc8, /* bswap %rax */
        0x0f, 0xc9,       /* bswap %ecx */
        0x49, 0x0f, 0xc8  /* bswap %r8 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSWAP);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX);
    ASSERT(d.op1.size == 8);

    ASSERT(hb_decode_x64(code + 3, sizeof(code) - 3, base + 3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSWAP);
    ASSERT(d.len == 2);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX);
    ASSERT(d.op1.size == 4);

    ASSERT(hb_decode_x64(code + 5, sizeof(code) - 5, base + 5, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_BSWAP);
    ASSERT(d.len == 3);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_R8);
    ASSERT(d.op1.size == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x0123456789abcdefULL;
    ctx->regs.x64.rcx = 0xaaaaaaaa11223344ULL;
    ctx->regs.x64.r8 = 0x0102030405060708ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0xefcdab8967452301ULL);
    ASSERT(ctx->regs.x64.rcx == 0x44332211ULL);
    ASSERT(ctx->regs.x64.r8 == 0x0807060504030201ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_legacy_high8_register_family_strlen_tail) {
    uint8_t test_dh[] = {0x84, 0xf6}; /* test %dh,%dh */
    uint8_t mov_dh[] = {0xb6, 0x34};  /* mov $0x34,%dh */
    uint8_t rex_mov_sil[] = {0x40, 0xb6, 0x34}; /* REX remaps encoding 6 to %sil. */
    uint64_t base = (uint64_t)(uintptr_t)test_dh;
    hb_decoded_t d;

    ASSERT(hb_decode_x64(test_dh, sizeof(test_dh), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_TEST);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.reg_offset == 1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RDX && d.op2.reg_offset == 1);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, test_dh, sizeof(test_dh), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)test_dh, sizeof(test_dh),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdx = 0x72; /* DL is non-zero, DH is zero: strlen tail must see ZF=1. */

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.zf == true);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    base = (uint64_t)(uintptr_t)mov_dh;
    ASSERT(hb_decode_x64(mov_dh, sizeof(mov_dh), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX && d.op1.reg_offset == 1);

    dec = hb_decoder_create(HB_ARCH_X64, mov_dh, sizeof(mov_dh), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)mov_dh, sizeof(mov_dh),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdx = 0x1122334455667788ULL;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0x1122334455663488ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    base = (uint64_t)(uintptr_t)rex_mov_sil;
    ASSERT(hb_decode_x64(rex_mov_sil, sizeof(rex_mov_sil), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RSI && d.op1.reg_offset == 0);

    tests_passed++;
}

TEST(interp_x64_byte_offset_propagates_through_flag_helpers) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);

    /* Direct helper regression: old hb_flags_exec_cmp_test() ignored AH/CH/DH/BH offsets. */
    ctx->regs.x64.rdx = 0x72; /* DL non-zero, DH zero. */
    hb_flags_exec_cmp_test(ctx, HB_IR_TEST, true, HB_REG_RDX, 1, true, HB_REG_RDX, 1, HB_SIZE_8);
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.zf == true);
    hb_context_destroy(ctx);

    uint8_t shl_ah[] = {0xd0, 0xe4}; /* shl %ah,1 */
    uint64_t base = (uint64_t)(uintptr_t)shl_ah;
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, shl_ah, sizeof(shl_ah), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)shl_ah, sizeof(shl_ah),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x1122334455668188ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x1122334455660288ULL);
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_CF | HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.cf == true);
    ASSERT(ctx->flags.zf == false);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t imul_ch[] = {0xf6, 0xed}; /* imul %ch */
    base = (uint64_t)(uintptr_t)imul_ch;
    dec = hb_decoder_create(HB_ARCH_X64, imul_ch, sizeof(imul_ch), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)imul_ch, sizeof(imul_ch),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x1122334455660004ULL; /* AL = 4 */
    ctx->regs.x64.rcx = 0xaabbccddeeff037fULL; /* CH = 3, CL = 0x7f */
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x112233445566000cULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cmpxchg_ch_bl[] = {0x0f, 0xb0, 0xdd}; /* cmpxchg %bl,%ch */
    base = (uint64_t)(uintptr_t)cmpxchg_ch_bl;
    dec = hb_decoder_create(HB_ARCH_X64, cmpxchg_ch_bl, sizeof(cmpxchg_ch_bl), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cmpxchg_ch_bl, sizeof(cmpxchg_ch_bl),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0x1122334455660005ULL; /* AL = CH, so write BL into CH. */
    ctx->regs.x64.rbx = 0x778899aabbccddeeULL; /* BL = 0xee */
    ctx->regs.x64.rcx = 0xaabbccddeeff057fULL; /* CH = 5, CL = 0x7f */
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 0x1122334455660005ULL);
    ASSERT(ctx->regs.x64.rcx == 0xaabbccddeeffee7fULL);
    ASSERT(hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF) == HB_OK);
    ASSERT(ctx->flags.zf == true);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    tests_passed++;
}

TEST(interp_x64_sse_visible_window_family_gaps) {
    hb_exec_result_t out;
    hb_decoder_t* dec;
    hb_ir_func_t* func;
    hb_context_t* ctx;

    uint8_t cvtdq2ps[] = {0x0f, 0x5b, 0xc1}; /* cvtdq2ps %xmm1,%xmm0 */
    uint64_t base = (uint64_t)(uintptr_t)cvtdq2ps;
    hb_decoded_t d;
    ASSERT(hb_decode_x64(cvtdq2ps, sizeof(cvtdq2ps), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTDQ2PS);

    dec = hb_decoder_create(HB_ARCH_X64, cvtdq2ps, sizeof(cvtdq2ps), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtdq2ps, sizeof(cvtdq2ps),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    int32_t dwords[4] = {1, -2, 3, 4};
    memcpy(ctx->regs.x64.xmm[1], dwords, sizeof(dwords));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t floats[4];
    memcpy(floats, ctx->regs.x64.xmm[0], sizeof(floats));
    ASSERT(floats[0] == 0x3f800000U);
    ASSERT(floats[1] == 0xc0000000U);
    ASSERT(floats[2] == 0x40400000U);
    ASSERT(floats[3] == 0x40800000U);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cvtps2dq[] = {0x66, 0x0f, 0x5b, 0xc1}; /* cvtps2dq %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)cvtps2dq;
    ASSERT(hb_decode_x64(cvtps2dq, sizeof(cvtps2dq), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTPS2DQ);
    dec = hb_decoder_create(HB_ARCH_X64, cvtps2dq, sizeof(cvtps2dq), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtps2dq, sizeof(cvtps2dq),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    memcpy(ctx->regs.x64.xmm[1], floats, sizeof(floats));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    int32_t out_dwords[4];
    memcpy(out_dwords, ctx->regs.x64.xmm[0], sizeof(out_dwords));
    ASSERT(out_dwords[0] == 1 && out_dwords[1] == -2 && out_dwords[2] == 3 && out_dwords[3] == 4);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cvtps2pd[] = {0x0f, 0x5a, 0xc1}; /* cvtps2pd %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)cvtps2pd;
    ASSERT(hb_decode_x64(cvtps2pd, sizeof(cvtps2pd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTPS2PD);
    dec = hb_decoder_create(HB_ARCH_X64, cvtps2pd, sizeof(cvtps2pd), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtps2pd, sizeof(cvtps2pd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t low_floats[2] = {0x3fc00000U, 0xc0000000U}; /* 1.5, -2.0 */
    memcpy(ctx->regs.x64.xmm[1], low_floats, sizeof(low_floats));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    double out_doubles[2];
    memcpy(out_doubles, ctx->regs.x64.xmm[0], sizeof(out_doubles));
    ASSERT(out_doubles[0] == 1.5);
    ASSERT(out_doubles[1] == -2.0);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cvtpd2ps[] = {0x66, 0x0f, 0x5a, 0xc1}; /* cvtpd2ps %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)cvtpd2ps;
    ASSERT(hb_decode_x64(cvtpd2ps, sizeof(cvtpd2ps), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTPD2PS);
    dec = hb_decoder_create(HB_ARCH_X64, cvtpd2ps, sizeof(cvtpd2ps), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtpd2ps, sizeof(cvtpd2ps),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    double packed_doubles[2] = {2.0, -3.5};
    memcpy(ctx->regs.x64.xmm[1], packed_doubles, sizeof(packed_doubles));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t packed_floats[4];
    memcpy(packed_floats, ctx->regs.x64.xmm[0], sizeof(packed_floats));
    ASSERT(packed_floats[0] == 0x40000000U);
    ASSERT(packed_floats[1] == 0xc0600000U);
    ASSERT(packed_floats[2] == 0);
    ASSERT(packed_floats[3] == 0);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cvtss2sd[] = {0xf3, 0x0f, 0x5a, 0xc1}; /* cvtss2sd %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)cvtss2sd;
    ASSERT(hb_decode_x64(cvtss2sd, sizeof(cvtss2sd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTSS2SD);
    dec = hb_decoder_create(HB_ARCH_X64, cvtss2sd, sizeof(cvtss2sd), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtss2sd, sizeof(cvtss2sd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][1] = 0x1122334455667788ULL;
    uint32_t four_float = 0x40800000U;
    memcpy(ctx->regs.x64.xmm[1], &four_float, sizeof(four_float));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    double scalar_double;
    memcpy(&scalar_double, ctx->regs.x64.xmm[0], sizeof(scalar_double));
    ASSERT(scalar_double == 4.0);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0x1122334455667788ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t cvtsd2ss[] = {0xf2, 0x0f, 0x5a, 0xc1}; /* cvtsd2ss %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)cvtsd2ss;
    ASSERT(hb_decode_x64(cvtsd2ss, sizeof(cvtsd2ss), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTSD2SS);
    dec = hb_decoder_create(HB_ARCH_X64, cvtsd2ss, sizeof(cvtsd2ss), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)cvtsd2ss, sizeof(cvtsd2ss),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaa55555555ULL;
    ctx->regs.x64.xmm[0][1] = 0x8877665544332211ULL;
    double six_point_five = 6.5;
    memcpy(ctx->regs.x64.xmm[1], &six_point_five, sizeof(six_point_five));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.xmm[0][0] == 0x40d00000U);
    ASSERT((uint32_t)(ctx->regs.x64.xmm[0][0] >> 32) == 0xaaaaaaaaU);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0x8877665544332211ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t maxsd[] = {0xf2, 0x0f, 0x5f, 0xc1}; /* maxsd %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)maxsd;
    ASSERT(hb_decode_x64(maxsd, sizeof(maxsd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MAXSD);
    dec = hb_decoder_create(HB_ARCH_X64, maxsd, sizeof(maxsd), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)maxsd, sizeof(maxsd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    double two = 2.0, three = 3.0;
    memcpy(ctx->regs.x64.xmm[0], &two, sizeof(two));
    ctx->regs.x64.xmm[0][1] = 0x0102030405060708ULL;
    memcpy(ctx->regs.x64.xmm[1], &three, sizeof(three));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    memcpy(&scalar_double, ctx->regs.x64.xmm[0], sizeof(scalar_double));
    ASSERT(scalar_double == 3.0);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0x0102030405060708ULL);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t minps[] = {0x0f, 0x5d, 0xc1}; /* minps %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)minps;
    ASSERT(hb_decode_x64(minps, sizeof(minps), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MINPS);
    dec = hb_decoder_create(HB_ARCH_X64, minps, sizeof(minps), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)minps, sizeof(minps),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t min_lhs[4] = {0x40a00000U, 0xc0000000U, 0x40e00000U, 0x41000000U}; /* 5,-2,7,8 */
    uint32_t min_rhs[4] = {0x40800000U, 0xbf800000U, 0x41100000U, 0x00000000U}; /* 4,-1,9,0 */
    memcpy(ctx->regs.x64.xmm[0], min_lhs, sizeof(min_lhs));
    memcpy(ctx->regs.x64.xmm[1], min_rhs, sizeof(min_rhs));
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    memcpy(packed_floats, ctx->regs.x64.xmm[0], sizeof(packed_floats));
    ASSERT(packed_floats[0] == 0x40800000U);
    ASSERT(packed_floats[1] == 0xc0000000U);
    ASSERT(packed_floats[2] == 0x40e00000U);
    ASSERT(packed_floats[3] == 0x00000000U);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t minpd[] = {0x66, 0x0f, 0x5d, 0xc1}; /* minpd sibling */
    uint8_t maxss[] = {0xf3, 0x0f, 0x5f, 0xc1}; /* maxss sibling */
    ASSERT(hb_decode_x64(minpd, sizeof(minpd), (uint64_t)(uintptr_t)minpd, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MINPD);
    ASSERT(hb_decode_x64(maxss, sizeof(maxss), (uint64_t)(uintptr_t)maxss, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MAXSS);

    uint8_t ucomisd[] = {0x66, 0x0f, 0x2e, 0xc1}; /* ucomisd %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)ucomisd;
    ASSERT(hb_decode_x64(ucomisd, sizeof(ucomisd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_COMISD);
    dec = hb_decoder_create(HB_ARCH_X64, ucomisd, sizeof(ucomisd), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)ucomisd, sizeof(ucomisd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x4000000000000000ULL; /* 2.0 */
    ctx->regs.x64.xmm[1][0] = 0x4008000000000000ULL; /* 3.0 */
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->flags.cf == true);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->flags.pf == false);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t paddq[] = {0x66, 0x0f, 0xd4, 0xc1}; /* paddq %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)paddq;
    ASSERT(hb_decode_x64(paddq, sizeof(paddq), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PADDQ);
    dec = hb_decoder_create(HB_ARCH_X64, paddq, sizeof(paddq), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)paddq, sizeof(paddq),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 5;
    ctx->regs.x64.xmm[0][1] = 10;
    ctx->regs.x64.xmm[1][0] = 7;
    ctx->regs.x64.xmm[1][1] = (uint64_t)-3;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 12);
    ASSERT(ctx->regs.x64.xmm[0][1] == 7);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t paddd[] = {0x66, 0x0f, 0xfe, 0xc1}; /* paddd %xmm1,%xmm0 */
    base = (uint64_t)(uintptr_t)paddd;
    ASSERT(hb_decode_x64(paddd, sizeof(paddd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PADDD);

    tests_passed++;
}

TEST(interp_x64_movd_cvtdq2pd_calc_cluster) {
    uint8_t code[] = {
        0x66, 0x0f, 0x6e, 0xc0, /* movd %eax, %xmm0 */
        0xf3, 0x0f, 0xe6, 0xc0  /* cvtdq2pd %xmm0, %xmm0 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 96;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == test_double_bits(96.0));
    ASSERT(ctx->regs.x64.xmm[0][1] == test_double_bits(0.0));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_psub_integer_family_smoke_helper_toolbar) {
    uint8_t psubd[] = {0x66, 0x0f, 0xfa, 0xf0}; /* psubd %xmm0, %xmm6 */
    uint64_t base = (uint64_t)(uintptr_t)psubd;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(psubd, sizeof(psubd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSUBD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM6);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, psubd, sizeof(psubd), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)psubd, sizeof(psubd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t lhs[4] = {10, 0, 0x80000000U, 5};
    uint32_t rhs[4] = {3, 1, 1, 7};
    memcpy(ctx->regs.x64.xmm[6], lhs, sizeof(lhs));
    memcpy(ctx->regs.x64.xmm[0], rhs, sizeof(rhs));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t got[4];
    memcpy(got, ctx->regs.x64.xmm[6], sizeof(got));
    ASSERT(got[0] == 7);
    ASSERT(got[1] == 0xffffffffU);
    ASSERT(got[2] == 0x7fffffffU);
    ASSERT(got[3] == 0xfffffffeU);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t psubb[] = {0x66, 0x0f, 0xf8, 0xc1}; /* psubb %xmm1, %xmm0 */
    uint8_t psubw[] = {0x66, 0x0f, 0xf9, 0xc1}; /* psubw %xmm1, %xmm0 */
    uint8_t psubq[] = {0x66, 0x0f, 0xfb, 0xc1}; /* psubq %xmm1, %xmm0 */
    ASSERT(hb_decode_x64(psubb, sizeof(psubb), (uint64_t)(uintptr_t)psubb, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSUBB);
    ASSERT(hb_decode_x64(psubw, sizeof(psubw), (uint64_t)(uintptr_t)psubw, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSUBW);
    ASSERT(hb_decode_x64(psubq, sizeof(psubq), (uint64_t)(uintptr_t)psubq, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSUBQ);

    tests_passed++;
}

TEST(interp_x64_pcmpgt_integer_family_smoke_helper_toolbar) {
    uint8_t code[] = {
        0x66, 0x0f, 0x66, 0x05, 0x00, 0x00, 0x00, 0x00 /* pcmpgtd disp32(%rip), %xmm0 */
    };
    int32_t rhs_data[4] = {3, 0, 0, INT32_MAX};
    uint64_t base = (uint64_t)(uintptr_t)code;
    int64_t rel = (int64_t)(uintptr_t)rhs_data - (int64_t)(base + sizeof(code));
    ASSERT(rel >= INT32_MIN && rel <= INT32_MAX);
    int32_t rel32 = (int32_t)rel;
    memcpy(code + 4, &rel32, sizeof(rel32));

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPGTD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_mem && d.op2.size == 16);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)rhs_data, sizeof(rhs_data),
                         HB_PERM_READ) == HB_OK);
    ctx->pc = base;
    int32_t lhs[4] = {5, -1, 0, (int32_t)0x80000000U};
    memcpy(ctx->regs.x64.xmm[0], lhs, sizeof(lhs));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t got[4];
    memcpy(got, ctx->regs.x64.xmm[0], sizeof(got));
    ASSERT(got[0] == 0xffffffffU);
    ASSERT(got[1] == 0);
    ASSERT(got[2] == 0);
    ASSERT(got[3] == 0);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t pcmpgtb[] = {0x66, 0x0f, 0x64, 0xc1}; /* pcmpgtb %xmm1, %xmm0 */
    uint8_t pcmpgtw[] = {0x66, 0x0f, 0x65, 0xc1}; /* pcmpgtw %xmm1, %xmm0 */
    ASSERT(hb_decode_x64(pcmpgtb, sizeof(pcmpgtb), (uint64_t)(uintptr_t)pcmpgtb, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPGTB);
    ASSERT(hb_decode_x64(pcmpgtw, sizeof(pcmpgtw), (uint64_t)(uintptr_t)pcmpgtw, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPGTW);

    tests_passed++;
}

TEST(interp_x64_movmsk_fp_sign_extract_family_smoke_helper_toolbar) {
    uint8_t movmskpd[] = {0x66, 0x0f, 0x50, 0xc0}; /* movmskpd %xmm0, %eax */
    uint64_t base = (uint64_t)(uintptr_t)movmskpd;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(movmskpd, sizeof(movmskpd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVMSKPD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RAX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, movmskpd, sizeof(movmskpd), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)movmskpd, sizeof(movmskpd),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xffffffffffffffffULL;
    ctx->regs.x64.xmm[0][0] = 0x8000000000000000ULL;
    ctx->regs.x64.xmm[0][1] = 0x7fffffffffffffffULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rax == 1);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t movmskps[] = {0x0f, 0x50, 0xc8}; /* movmskps %xmm0, %ecx */
    base = (uint64_t)(uintptr_t)movmskps;
    ASSERT(hb_decode_x64(movmskps, sizeof(movmskps), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVMSKPS);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RCX && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    dec = hb_decoder_create(HB_ARCH_X64, movmskps, sizeof(movmskps), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)movmskps, sizeof(movmskps),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = 0xffffffffffffffffULL;
    uint32_t lanes[4] = {0x80000000U, 0x7fffffffU, 0xffffffffU, 0};
    memcpy(ctx->regs.x64.xmm[0], lanes, sizeof(lanes));

    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rcx == 5);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    tests_passed++;
}

TEST(interp_x64_calc_sse2_divsd_mulsd_cluster) {
    struct {
        uint8_t code[32];
        double divisor;
        double multiplier;
    } block __attribute__((aligned(16)));
    memset(&block, 0xcc, sizeof(block));

    uint64_t base = (uint64_t)(uintptr_t)block.code;
    int32_t div_disp = (int32_t)((uint64_t)(uintptr_t)&block.divisor - (base + 16));
    int32_t mul_disp = (int32_t)((uint64_t)(uintptr_t)&block.multiplier - (base + 24));

    block.code[0] = 0x66; /* movd %eax, %xmm0 */
    block.code[1] = 0x0f;
    block.code[2] = 0x6e;
    block.code[3] = 0xc0;
    block.code[4] = 0xf3; /* cvtdq2pd %xmm0, %xmm0 */
    block.code[5] = 0x0f;
    block.code[6] = 0xe6;
    block.code[7] = 0xc0;
    block.code[8] = 0xf2; /* divsd divisor(%rip), %xmm0 */
    block.code[9] = 0x0f;
    block.code[10] = 0x5e;
    block.code[11] = 0x05;
    memcpy(&block.code[12], &div_disp, sizeof(div_disp));
    block.code[16] = 0xf2; /* mulsd multiplier(%rip), %xmm0 */
    block.code[17] = 0x0f;
    block.code[18] = 0x59;
    block.code[19] = 0x05;
    memcpy(&block.code[20], &mul_disp, sizeof(mul_disp));
    block.divisor = 2.0;
    block.multiplier = 3.0;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, 24, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 96;
    ctx->regs.x64.xmm[0][1] = 0x1122334455667788ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == test_double_bits(144.0));
    ASSERT(ctx->regs.x64.xmm[0][1] == test_double_bits(0.0));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_addsd_subsd_scalar_double) {
    uint8_t code[] = {
        0xf2, 0x0f, 0x58, 0xc1, /* addsd %xmm1, %xmm0 */
        0xf2, 0x0f, 0x5c, 0xc2  /* subsd %xmm2, %xmm0 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = test_double_bits(10.0);
    ctx->regs.x64.xmm[0][1] = 0xabcdefabcdefabcdULL;
    ctx->regs.x64.xmm[1][0] = test_double_bits(2.5);
    ctx->regs.x64.xmm[2][0] = test_double_bits(1.0);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == test_double_bits(11.5));
    ASSERT(ctx->regs.x64.xmm[0][1] == 0xabcdefabcdefabcdULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movdqa_xmm_store_calc) {
    uint8_t code[] = {0x66, 0x0f, 0x7f, 0x00}; /* movdqa %xmm0, (%rax) */
    uint64_t dst[2] = {0, 0};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.xmm[0][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[0][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(dst[0] == 0x0123456789abcdefULL);
    ASSERT(dst[1] == 0xfedcba9876543210ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movdqu_unaligned_xmm_move_blit_path) {
    uint8_t code[] = {
        0xf3, 0x0f, 0x6f, 0x0b,       /* movdqu (%rbx), %xmm1 */
        0xf3, 0x0f, 0x7f, 0x48, 0x01  /* movdqu %xmm1, 1(%rax) */
    };
    uint8_t src[32];
    uint8_t dst[32];
    uint64_t base = (uint64_t)(uintptr_t)code;

    for (unsigned i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(0xa0 + i);
    memset(dst, 0, sizeof(dst));

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, 4, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1 && d.op1.size == 16);
    ASSERT(d.op2.is_mem && d.op2.size == 16);
    ASSERT(hb_decode_x64(code + 4, 5, base + 4, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 16);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)src, sizeof(src),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)(src + 1);
    ctx->regs.x64.rax = (uint64_t)(uintptr_t)dst;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(memcmp(dst + 1, src + 1, 16) == 0);
    ASSERT(dst[0] == 0 && dst[17] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_scalar_sse_move_store_width_notepadpp_paint) {
    uint8_t code[] = {
        0xf2, 0x0f, 0x11, 0x47, 0x08, /* movsd %xmm0, 0x08(%rdi) */
        0xf3, 0x0f, 0x11, 0x4f, 0x18, /* movss %xmm1, 0x18(%rdi) */
        0x0f, 0x11, 0x57, 0x30        /* movups %xmm2, 0x30(%rdi) */
    };
    uint8_t dst[80];
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, 5, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 8);
    ASSERT(hb_decode_x64(code + 5, 5, base + 5, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 4);
    ASSERT(hb_decode_x64(code + 10, 4, base + 10, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM2 && d.op2.size == 16);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)dst, sizeof(dst),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    memset(dst, 0xaa, sizeof(dst));
    ctx->pc = base;
    ctx->regs.x64.rdi = (uint64_t)(uintptr_t)dst;
    ctx->regs.x64.xmm[0][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[0][1] = 0x1111111111111111ULL;
    ctx->regs.x64.xmm[1][0] = 0x22222222deadbeefULL;
    ctx->regs.x64.xmm[1][1] = 0x3333333333333333ULL;
    ctx->regs.x64.xmm[2][0] = 0x4444444455555555ULL;
    ctx->regs.x64.xmm[2][1] = 0x6666666677777777ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint64_t got64 = 0;
    uint32_t got32 = 0;
    memcpy(&got64, dst + 0x08, sizeof(got64));
    memcpy(&got32, dst + 0x18, sizeof(got32));
    ASSERT(got64 == 0x0123456789abcdefULL);
    ASSERT(got32 == 0xdeadbeefU);
    for (size_t i = 0x10; i < 0x18; i++) ASSERT(dst[i] == 0xaa);
    for (size_t i = 0x1c; i < 0x20; i++) ASSERT(dst[i] == 0xaa);
    memcpy(&got64, dst + 0x30, sizeof(got64));
    ASSERT(got64 == 0x4444444455555555ULL);
    memcpy(&got64, dst + 0x38, sizeof(got64));
    ASSERT(got64 == 0x6666666677777777ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_scalar_sse_mem_load_zeroes_upper_reg_move_preserves_upper) {
    uint8_t code[] = {
        0xf3, 0x0f, 0x10, 0x00, /* movss (%rax), %xmm0 */
        0xf3, 0x0f, 0x10, 0xc8  /* movss %xmm0, %xmm1 */
    };
    uint32_t src = 0x55667788u;
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);
    ASSERT(func->cfg && func->cfg->entry && func->cfg->entry->instr_count >= 2);
    ASSERT(func->cfg->entry->instrs[0].op == HB_IR_LOAD);
    ASSERT(func->cfg->entry->instrs[0].zero_upper);
    ASSERT(!func->cfg->entry->instrs[1].zero_upper);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&src, sizeof(src),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rip = base;
    ctx->regs.x64.rax = (uint64_t)(uintptr_t)&src;
    ctx->regs.x64.xmm[0][0] = 0xaaaabbbbccccddddULL;
    ctx->regs.x64.xmm[0][1] = 0xeeeeffff11112222ULL;
    ctx->regs.x64.xmm[1][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[1][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == src);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);
    ASSERT(ctx->regs.x64.xmm[1][0] == 0x0123456755667788ULL);
    ASSERT(ctx->regs.x64.xmm[1][1] == 0xfedcba9876543210ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movlps_low_qword_family_rsaenh) {
    uint8_t code[] = {
        0x0f, 0x13, 0x07,             /* movlps %xmm0, (%rdi) */
        0x66, 0x0f, 0x13, 0x4f, 0x08, /* movlpd %xmm1, 0x08(%rdi) */
        0x0f, 0x12, 0x10              /* movlps (%rax), %xmm2 */
    };
    struct {
        uint8_t dst[32];
        uint64_t src;
    } data;
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, 3, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0 && d.op2.size == 8);
    ASSERT(hb_decode_x64(code + 3, 5, base + 3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 8 && d.op1.mem.disp == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1 && d.op2.size == 8);
    ASSERT(hb_decode_x64(code + 8, 3, base + 8, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SSE_MOV);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM2 && d.op1.size == 8);
    ASSERT(d.op2.is_mem && d.op2.size == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&data, sizeof(data),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    memset(&data, 0xaa, sizeof(data));
    data.src = 0x1020304050607080ULL;
    ctx->pc = base;
    ctx->regs.x64.rdi = (uint64_t)(uintptr_t)data.dst;
    ctx->regs.x64.rax = (uint64_t)(uintptr_t)&data.src;
    ctx->regs.x64.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x64.xmm[0][1] = 0x99aabbccddeeff00ULL;
    ctx->regs.x64.xmm[1][0] = 0x8877665544332211ULL;
    ctx->regs.x64.xmm[1][1] = 0x0102030405060708ULL;
    ctx->regs.x64.xmm[2][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[2][1] = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint64_t got = 0;
    memcpy(&got, data.dst, sizeof(got));
    ASSERT(got == 0x1122334455667788ULL);
    memcpy(&got, data.dst + 8, sizeof(got));
    ASSERT(got == 0x8877665544332211ULL);
    memcpy(&got, data.dst + 16, sizeof(got));
    ASSERT(got == 0xaaaaaaaaaaaaaaaaULL);
    ASSERT(ctx->regs.x64.xmm[2][0] == data.src);
    ASSERT(ctx->regs.x64.xmm[2][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_movhps_movlhps_qword_lane_family_unityplayer) {
    uint8_t code[] = {
        0x0f, 0x16, 0xf0,       /* movlhps %xmm0, %xmm6 */
        0x0f, 0x12, 0xd8,       /* movhlps %xmm0, %xmm3 */
        0x0f, 0x16, 0x0f,       /* movhps (%rdi), %xmm1 */
        0x0f, 0x17, 0x57, 0x08  /* movhps %xmm2, 0x08(%rdi) */
    };
    uint64_t slots[2] = {0x1111222233334444ULL, 0xaaaaaaaaaaaaaaaaULL};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, 3, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVLHPS);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM6);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);
    ASSERT(hb_decode_x64(code + 3, 3, base + 3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVHLPS);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM3);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);
    ASSERT(hb_decode_x64(code + 6, 3, base + 6, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVHPS);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1);
    ASSERT(d.op2.is_mem && d.op2.size == 8);
    ASSERT(hb_decode_x64(code + 9, 4, base + 9, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOVHPS);
    ASSERT(d.op1.is_mem && d.op1.size == 8);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM2);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)slots, sizeof(slots),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdi = (uint64_t)(uintptr_t)slots;
    ctx->regs.x64.xmm[0][0] = 0x0102030405060708ULL;
    ctx->regs.x64.xmm[0][1] = 0x1112131415161718ULL;
    ctx->regs.x64.xmm[1][0] = 0x2122232425262728ULL;
    ctx->regs.x64.xmm[1][1] = 0x3132333435363738ULL;
    ctx->regs.x64.xmm[2][0] = 0x4142434445464748ULL;
    ctx->regs.x64.xmm[2][1] = 0x5152535455565758ULL;
    ctx->regs.x64.xmm[3][0] = 0x6162636465666768ULL;
    ctx->regs.x64.xmm[3][1] = 0x7172737475767778ULL;
    ctx->regs.x64.xmm[6][0] = 0x8182838485868788ULL;
    ctx->regs.x64.xmm[6][1] = 0x9192939495969798ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[6][0] == 0x8182838485868788ULL);
    ASSERT(ctx->regs.x64.xmm[6][1] == 0x0102030405060708ULL);
    ASSERT(ctx->regs.x64.xmm[3][0] == 0x1112131415161718ULL);
    ASSERT(ctx->regs.x64.xmm[3][1] == 0x7172737475767778ULL);
    ASSERT(ctx->regs.x64.xmm[1][0] == 0x2122232425262728ULL);
    ASSERT(ctx->regs.x64.xmm[1][1] == 0x1111222233334444ULL);
    ASSERT(slots[1] == 0x5152535455565758ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_addpd_subpd_notepadpp_callback_block) {
    uint8_t code[] = {
        0xf2, 0x0f, 0x10, 0x53, 0x10, /* movsd 0x10(%rbx), %xmm2 */
        0x0f, 0x10, 0x07,             /* movups (%rdi), %xmm0 */
        0x0f, 0x10, 0x4f, 0x10,       /* movups 0x10(%rdi), %xmm1 */
        0x66, 0x0f, 0x14, 0xd2,       /* unpcklpd %xmm2, %xmm2 */
        0x66, 0x0f, 0x58, 0xc2,       /* addpd %xmm2, %xmm0 */
        0x66, 0x0f, 0x5c, 0xca,       /* subpd %xmm2, %xmm1 */
        0x0f, 0x11, 0x47, 0x20,       /* movups %xmm0, 0x20(%rdi) */
        0x0f, 0x11, 0x4f, 0x30        /* movups %xmm1, 0x30(%rdi) */
    };
    struct {
        uint8_t vectors[80];
        uint8_t scale[32];
    } mem;
    uint64_t base = (uint64_t)(uintptr_t)code;

    memset(&mem, 0xaa, sizeof(mem));
    uint64_t scalar = test_double_bits(2.0);
    uint64_t v0[2] = {test_double_bits(10.0), test_double_bits(20.0)};
    uint64_t v1[2] = {test_double_bits(30.0), test_double_bits(40.0)};
    memset(mem.scale, 0, sizeof(mem.scale));
    memcpy(mem.scale + 0x10, &scalar, sizeof(scalar));
    memcpy(mem.vectors, v0, sizeof(v0));
    memcpy(mem.vectors + 0x10, v1, sizeof(v1));

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code + 16, 4, base + 16, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADDPD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0 && d.op1.size == 16);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM2 && d.op2.size == 16);
    ASSERT(hb_decode_x64(code + 20, 4, base + 20, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SUBPD);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&mem, sizeof(mem),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdi = (uint64_t)(uintptr_t)mem.vectors;
    ctx->regs.x64.rbx = (uint64_t)(uintptr_t)mem.scale;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint64_t got[2];
    memcpy(got, mem.vectors + 0x20, sizeof(got));
    ASSERT(got[0] == test_double_bits(12.0));
    ASSERT(got[1] == test_double_bits(22.0));
    memcpy(got, mem.vectors + 0x30, sizeof(got));
    ASSERT(got[0] == test_double_bits(28.0));
    ASSERT(got[1] == test_double_bits(38.0));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_addps_subps_addss_subss_family) {
    uint8_t code[] = {
        0x0f, 0x58, 0xc1,             /* addps %xmm1, %xmm0 */
        0x0f, 0x5c, 0xd3,             /* subps %xmm3, %xmm2 */
        0xf3, 0x0f, 0x58, 0xe5,       /* addss %xmm5, %xmm4 */
        0xf3, 0x0f, 0x5c, 0xf7        /* subss %xmm7, %xmm6 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, 3, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADDPS);
    ASSERT(hb_decode_x64(code + 3, 3, base + 3, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SUBPS);
    ASSERT(hb_decode_x64(code + 6, 4, base + 6, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_ADDSS);
    ASSERT(hb_decode_x64(code + 10, 4, base + 10, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SUBSS);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint32_t xmm0[4] = {test_float_bits(1.0f), test_float_bits(2.0f),
                        test_float_bits(3.0f), test_float_bits(4.0f)};
    uint32_t xmm1[4] = {test_float_bits(10.0f), test_float_bits(20.0f),
                        test_float_bits(30.0f), test_float_bits(40.0f)};
    uint32_t xmm2[4] = {test_float_bits(50.0f), test_float_bits(60.0f),
                        test_float_bits(70.0f), test_float_bits(80.0f)};
    uint32_t xmm3[4] = {test_float_bits(5.0f), test_float_bits(6.0f),
                        test_float_bits(7.0f), test_float_bits(8.0f)};
    uint32_t xmm4[4] = {test_float_bits(9.0f), test_float_bits(100.0f),
                        test_float_bits(200.0f), test_float_bits(300.0f)};
    uint32_t xmm5[4] = {test_float_bits(3.0f), test_float_bits(400.0f),
                        test_float_bits(500.0f), test_float_bits(600.0f)};
    uint32_t xmm6[4] = {test_float_bits(9.0f), test_float_bits(700.0f),
                        test_float_bits(800.0f), test_float_bits(900.0f)};
    uint32_t xmm7[4] = {test_float_bits(4.0f), test_float_bits(1000.0f),
                        test_float_bits(1100.0f), test_float_bits(1200.0f)};
    memcpy(ctx->regs.x64.xmm[0], xmm0, sizeof(xmm0));
    memcpy(ctx->regs.x64.xmm[1], xmm1, sizeof(xmm1));
    memcpy(ctx->regs.x64.xmm[2], xmm2, sizeof(xmm2));
    memcpy(ctx->regs.x64.xmm[3], xmm3, sizeof(xmm3));
    memcpy(ctx->regs.x64.xmm[4], xmm4, sizeof(xmm4));
    memcpy(ctx->regs.x64.xmm[5], xmm5, sizeof(xmm5));
    memcpy(ctx->regs.x64.xmm[6], xmm6, sizeof(xmm6));
    memcpy(ctx->regs.x64.xmm[7], xmm7, sizeof(xmm7));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint32_t got[4];
    memcpy(got, ctx->regs.x64.xmm[0], sizeof(got));
    ASSERT(got[0] == test_float_bits(11.0f));
    ASSERT(got[1] == test_float_bits(22.0f));
    ASSERT(got[2] == test_float_bits(33.0f));
    ASSERT(got[3] == test_float_bits(44.0f));
    memcpy(got, ctx->regs.x64.xmm[2], sizeof(got));
    ASSERT(got[0] == test_float_bits(45.0f));
    ASSERT(got[1] == test_float_bits(54.0f));
    ASSERT(got[2] == test_float_bits(63.0f));
    ASSERT(got[3] == test_float_bits(72.0f));
    memcpy(got, ctx->regs.x64.xmm[4], sizeof(got));
    ASSERT(got[0] == test_float_bits(12.0f));
    ASSERT(got[1] == test_float_bits(100.0f));
    ASSERT(got[2] == test_float_bits(200.0f));
    ASSERT(got[3] == test_float_bits(300.0f));
    memcpy(got, ctx->regs.x64.xmm[6], sizeof(got));
    ASSERT(got[0] == test_float_bits(5.0f));
    ASSERT(got[1] == test_float_bits(700.0f));
    ASSERT(got[2] == test_float_bits(800.0f));
    ASSERT(got[3] == test_float_bits(900.0f));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_mov_imm32_rip_relative_store_calc_crt_state) {
    struct {
        uint8_t code[16];
        uint32_t state;
    } block;
    memset(&block, 0xcc, sizeof(block));

    uint64_t base = (uint64_t)(uintptr_t)block.code;
    int32_t disp = (int32_t)((uint64_t)(uintptr_t)&block.state - (base + 10));
    block.code[0] = 0xc7; /* movl $1, state(%rip) */
    block.code[1] = 0x05;
    memcpy(&block.code[2], &disp, sizeof(disp));
    block.code[6] = 0x01;
    block.code[7] = 0x00;
    block.code[8] = 0x00;
    block.code[9] = 0x00;
    block.state = 0;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(block.code, 10, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.len == 10);
    ASSERT(d.op1.is_mem);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op1.mem.rip_relative);
    ASSERT(d.op2.is_imm && d.op2.imm == 1);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, 10, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(block.state == 1);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_rip_relative_trailing_immediate_family) {
    struct {
        uint8_t code[16];
        uint32_t cell;
    } block;
    uint64_t base, rip_base, target;
    int32_t disp;

#define CHECK_RIPREL_IMM(bytes_len, mem_slot, fill_code) do { \
        memset(&block, 0xcc, sizeof(block)); \
        base = (uint64_t)(uintptr_t)block.code; \
        disp = (int32_t)((uint64_t)(uintptr_t)&block.cell - (base + (bytes_len))); \
        do { fill_code; } while (0); \
        ASSERT(decode_riprel_target(block.code, (bytes_len), base, (mem_slot), &rip_base, &target)); \
        ASSERT(rip_base == base + (bytes_len)); \
        ASSERT(target == (uint64_t)(uintptr_t)&block.cell); \
    } while (0)

    CHECK_RIPREL_IMM(7, 1, {
        block.code[0] = 0xc6; block.code[1] = 0x05; /* movb imm8, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 0x7f;
    });
    CHECK_RIPREL_IMM(10, 1, {
        block.code[0] = 0xc7; block.code[1] = 0x05; /* movl imm32, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 1;
    });
    CHECK_RIPREL_IMM(10, 1, {
        block.code[0] = 0x81; block.code[1] = 0x05; /* addl imm32, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 2;
    });
    CHECK_RIPREL_IMM(7, 1, {
        block.code[0] = 0x83; block.code[1] = 0x2d; /* subl imm8, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 1;
    });
    CHECK_RIPREL_IMM(10, 2, {
        block.code[0] = 0x69; block.code[1] = 0x05; /* imull imm32, disp32(%rip), eax */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 3;
    });
    CHECK_RIPREL_IMM(7, 2, {
        block.code[0] = 0x6b; block.code[1] = 0x05; /* imull imm8, disp32(%rip), eax */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 3;
    });
    CHECK_RIPREL_IMM(7, 1, {
        block.code[0] = 0xc1; block.code[1] = 0x25; /* shll imm8, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 3;
    });
    CHECK_RIPREL_IMM(7, 1, {
        block.code[0] = 0xf6; block.code[1] = 0x05; /* testb imm8, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 0xff;
    });
    CHECK_RIPREL_IMM(10, 1, {
        block.code[0] = 0xf7; block.code[1] = 0x05; /* testl imm32, disp32(%rip) */
        memcpy(&block.code[2], &disp, 4); block.code[6] = 0xff;
    });
    CHECK_RIPREL_IMM(8, 1, {
        block.code[0] = 0x0f; block.code[1] = 0xba; block.code[2] = 0x35; /* btrl imm8, disp32(%rip) */
        memcpy(&block.code[3], &disp, 4); block.code[7] = 3;
    });

#undef CHECK_RIPREL_IMM
    tests_passed++;
}

TEST(interp_x64_xorps_zero_startup_block) {
    uint8_t code[] = {0x0f, 0x57, 0xc0}; /* xorps %xmm0, %xmm0 */
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XORPS);
    ASSERT(d.len == 3);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[0][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_pxor_zero_calc_vector_path) {
    uint8_t code[] = {0x66, 0x0f, 0xef, 0xc0}; /* pxor %xmm0, %xmm0 */
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PXOR);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[0][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_xmm_bitwise_logical_family_notepadpp) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
    } cases[] = {
        {{0x0f, 0x54, 0xc1, 0x00}, 3, HB_INS_XMM_AND},
        {{0x0f, 0x55, 0xc1, 0x00}, 3, HB_INS_XMM_ANDN},
        {{0x0f, 0x56, 0xc1, 0x00}, 3, HB_INS_XMM_OR},
        {{0x0f, 0x57, 0xc1, 0x00}, 3, HB_INS_XORPS},
        {{0x66, 0x0f, 0x54, 0xc1}, 4, HB_INS_XMM_AND},
        {{0x66, 0x0f, 0x55, 0xc1}, 4, HB_INS_XMM_ANDN},
        {{0x66, 0x0f, 0x56, 0xc1}, 4, HB_INS_XMM_OR},
        {{0x66, 0x0f, 0x57, 0xc1}, 4, HB_INS_XORPS},
        {{0x66, 0x0f, 0xdb, 0xc1}, 4, HB_INS_XMM_AND},
        {{0x66, 0x0f, 0xdf, 0xc1}, 4, HB_INS_XMM_ANDN},
        {{0x66, 0x0f, 0xeb, 0xc1}, 4, HB_INS_XMM_OR},
        {{0x66, 0x0f, 0xef, 0xc1}, 4, HB_INS_PXOR},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].code, cases[i].len, (uint64_t)(uintptr_t)cases[i].code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t code[] = {
        0x0f, 0x56, 0xc1,       /* orps  %xmm1, %xmm0 */
        0x66, 0x0f, 0x55, 0xd3, /* andnpd %xmm3, %xmm2 */
        0x66, 0x0f, 0xdb, 0xe5, /* pand  %xmm5, %xmm4 */
        0x66, 0x0f, 0xeb, 0xf7  /* por   %xmm7, %xmm6 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x0f0f0f0f0f0f0f0fULL;
    ctx->regs.x64.xmm[0][1] = 0xf000f000f000f000ULL;
    ctx->regs.x64.xmm[1][0] = 0x3333333333333333ULL;
    ctx->regs.x64.xmm[1][1] = 0x0ff00ff00ff00ff0ULL;
    ctx->regs.x64.xmm[2][0] = 0xff00ff00ff00ff00ULL;
    ctx->regs.x64.xmm[2][1] = 0xff00ff00ff00ff00ULL;
    ctx->regs.x64.xmm[3][0] = 0x0f0f0f0f0f0f0f0fULL;
    ctx->regs.x64.xmm[3][1] = 0x0f0f0f0f0f0f0f0fULL;
    ctx->regs.x64.xmm[4][0] = 0xffff0000ffff0000ULL;
    ctx->regs.x64.xmm[4][1] = 0xffff0000ffff0000ULL;
    ctx->regs.x64.xmm[5][0] = 0x00ff00ff00ff00ffULL;
    ctx->regs.x64.xmm[5][1] = 0x00ff00ff00ff00ffULL;
    ctx->regs.x64.xmm[6][0] = 0x1111000011110000ULL;
    ctx->regs.x64.xmm[6][1] = 0x1111000011110000ULL;
    ctx->regs.x64.xmm[7][0] = 0x0000222200002222ULL;
    ctx->regs.x64.xmm[7][1] = 0x0000222200002222ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0x3f3f3f3f3f3f3f3fULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0xfff0fff0fff0fff0ULL);
    ASSERT(ctx->regs.x64.xmm[2][0] == 0x000f000f000f000fULL);
    ASSERT(ctx->regs.x64.xmm[2][1] == 0x000f000f000f000fULL);
    ASSERT(ctx->regs.x64.xmm[4][0] == 0x00ff000000ff0000ULL);
    ASSERT(ctx->regs.x64.xmm[4][1] == 0x00ff000000ff0000ULL);
    ASSERT(ctx->regs.x64.xmm[6][0] == 0x1111222211112222ULL);
    ASSERT(ctx->regs.x64.xmm[6][1] == 0x1111222211112222ULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_unpck_fp_family_callback_path) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
    } cases[] = {
        {{0x0f, 0x14, 0xc1, 0x00}, 3, HB_INS_UNPCKLPS},
        {{0x66, 0x0f, 0x14, 0xc1}, 4, HB_INS_UNPCKLPD},
        {{0x0f, 0x15, 0xc1, 0x00}, 3, HB_INS_UNPCKHPS},
        {{0x66, 0x0f, 0x15, 0xc1}, 4, HB_INS_UNPCKHPD},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].code, cases[i].len, (uint64_t)(uintptr_t)cases[i].code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t code[] = {
        0x66, 0x0f, 0x14, 0xc1, /* unpcklpd %xmm1, %xmm0 */
        0x0f, 0x15, 0xd3        /* unpckhps %xmm3, %xmm2 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint8_t x0[16], x1[16], x2[16], x3[16];
    for (int i = 0; i < 16; i++) {
        x0[i] = (uint8_t)i;
        x1[i] = (uint8_t)(0x80 + i);
        x2[i] = (uint8_t)(0x20 + i);
        x3[i] = (uint8_t)(0xa0 + i);
    }
    memcpy(ctx->regs.x64.xmm[0], x0, sizeof(x0));
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[2], x2, sizeof(x2));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint8_t got[16];
    uint8_t expected_lpd[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
    };
    uint8_t expected_hps[16] = {
        0x28, 0x29, 0x2a, 0x2b, 0xa8, 0xa9, 0xaa, 0xab,
        0x2c, 0x2d, 0x2e, 0x2f, 0xac, 0xad, 0xae, 0xaf
    };
    memcpy(got, ctx->regs.x64.xmm[0], sizeof(got));
    ASSERT(memcmp(got, expected_lpd, sizeof(got)) == 0);
    memcpy(got, ctx->regs.x64.xmm[2], sizeof(got));
    ASSERT(memcmp(got, expected_hps, sizeof(got)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_punpcklqdq_notepadpp_broadcast) {
    uint8_t code[] = {0x66, 0x0f, 0x6c, 0xc0}; /* punpcklqdq %xmm0, %xmm0 */
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PUNPCKLQDQ);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x0123456789abcdefULL;
    ctx->regs.x64.xmm[0][1] = 0xfedcba9876543210ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0x0123456789abcdefULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0x0123456789abcdefULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_punpck_integer_family_notepadpp_vector_unpack) {
    struct {
        uint8_t op2;
        int opcode;
    } cases[] = {
        {0x60, HB_INS_PUNPCKLBW},  {0x61, HB_INS_PUNPCKLWD},
        {0x62, HB_INS_PUNPCKLDQ},  {0x6c, HB_INS_PUNPCKLQDQ},
        {0x68, HB_INS_PUNPCKHBW},  {0x69, HB_INS_PUNPCKHWD},
        {0x6a, HB_INS_PUNPCKHDQ},  {0x6d, HB_INS_PUNPCKHQDQ},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t dec_code[] = {0x66, 0x0f, cases[i].op2, 0xc1};
        hb_decoded_t d;
        ASSERT(hb_decode_x64(dec_code, sizeof(dec_code), (uint64_t)(uintptr_t)dec_code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t code[] = {
        0x66, 0x0f, 0x60, 0xc1, /* punpcklbw %xmm1, %xmm0 */
        0x66, 0x0f, 0x69, 0xd3  /* punpckhwd %xmm3, %xmm2 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint8_t x0[16], x1[16], x2[16], x3[16];
    for (int i = 0; i < 16; i++) {
        x0[i] = (uint8_t)i;
        x1[i] = (uint8_t)(0x80 + i);
        x2[i] = (uint8_t)(0x10 + i);
        x3[i] = (uint8_t)(0x90 + i);
    }
    memcpy(ctx->regs.x64.xmm[0], x0, sizeof(x0));
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[2], x2, sizeof(x2));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint8_t got[16];
    uint8_t expected_low_bw[16] = {
        0x00, 0x80, 0x01, 0x81, 0x02, 0x82, 0x03, 0x83,
        0x04, 0x84, 0x05, 0x85, 0x06, 0x86, 0x07, 0x87
    };
    uint8_t expected_high_wd[16] = {
        0x18, 0x19, 0x98, 0x99, 0x1a, 0x1b, 0x9a, 0x9b,
        0x1c, 0x1d, 0x9c, 0x9d, 0x1e, 0x1f, 0x9e, 0x9f
    };
    memcpy(got, ctx->regs.x64.xmm[0], sizeof(got));
    ASSERT(memcmp(got, expected_low_bw, sizeof(got)) == 0);
    memcpy(got, ctx->regs.x64.xmm[2], sizeof(got));
    ASSERT(memcmp(got, expected_high_wd, sizeof(got)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_pack_integer_saturation_family_blit_path) {
    struct {
        uint8_t op2;
        int opcode;
    } cases[] = {
        {0x63, HB_INS_PACKSSWB},
        {0x67, HB_INS_PACKUSWB},
        {0x6b, HB_INS_PACKSSDW},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t dec_code[] = {0x66, 0x0f, cases[i].op2, 0xc1};
        hb_decoded_t d;
        ASSERT(hb_decode_x64(dec_code, sizeof(dec_code), (uint64_t)(uintptr_t)dec_code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t code[] = {
        0x66, 0x0f, 0x63, 0xc1, /* packsswb %xmm1, %xmm0 */
        0x66, 0x0f, 0x67, 0xd3, /* packuswb %xmm3, %xmm2 */
        0x66, 0x0f, 0x6b, 0xe5  /* packssdw %xmm5, %xmm4 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    int16_t x0[8] = {-200, -129, -128, -1, 0, 1, 127, 128};
    int16_t x1[8] = {255, 300, -32768, 32767, 42, -42, 130, -130};
    int16_t x2[8] = {-1, 0, 1, 254, 255, 256, 300, 32767};
    int16_t x3[8] = {-32768, -5, 42, 128, 512, 1024, 200, 255};
    int32_t x4[4] = {-40000, -32769, -32768, -1};
    int32_t x5[4] = {0, 1, 32767, 32768};
    memcpy(ctx->regs.x64.xmm[0], x0, sizeof(x0));
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[2], x2, sizeof(x2));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));
    memcpy(ctx->regs.x64.xmm[4], x4, sizeof(x4));
    memcpy(ctx->regs.x64.xmm[5], x5, sizeof(x5));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint8_t expected_packsswb[16] = {
        0x80, 0x80, 0x80, 0xff, 0x00, 0x01, 0x7f, 0x7f,
        0x7f, 0x7f, 0x80, 0x7f, 0x2a, 0xd6, 0x7f, 0x80
    };
    uint8_t expected_packuswb[16] = {
        0x00, 0x00, 0x01, 0xfe, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x2a, 0x80, 0xff, 0xff, 0xc8, 0xff
    };
    int16_t expected_packssdw[8] = {
        -32768, -32768, -32768, -1, 0, 1, 32767, 32767
    };
    ASSERT(memcmp(ctx->regs.x64.xmm[0], expected_packsswb, sizeof(expected_packsswb)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[2], expected_packuswb, sizeof(expected_packuswb)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[4], expected_packssdw, sizeof(expected_packssdw)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_pmul_integer_family_blit_path) {
    struct {
        uint8_t op2;
        int opcode;
    } cases[] = {
        {0xd5, HB_INS_PMULLW},
        {0xe5, HB_INS_PMULHW},
        {0xe4, HB_INS_PMULHUW},
        {0xf5, HB_INS_PMADDWD},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t dec_code[] = {0x66, 0x0f, cases[i].op2, 0xc1};
        hb_decoded_t d;
        ASSERT(hb_decode_x64(dec_code, sizeof(dec_code), (uint64_t)(uintptr_t)dec_code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t code[] = {
        0x66, 0x0f, 0xd5, 0xc1, /* pmullw  %xmm1, %xmm0 */
        0x66, 0x0f, 0xe5, 0xd3, /* pmulhw  %xmm3, %xmm2 */
        0x66, 0x0f, 0xe4, 0xe5, /* pmulhuw %xmm5, %xmm4 */
        0x66, 0x0f, 0xf5, 0xf7  /* pmaddwd %xmm7, %xmm6 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    int16_t x0[8] = {1000, -1000, 30000, -30000, 32767, -32768, 1234, -1234};
    int16_t x1[8] = {2, 3, 2, 2, 2, 2, -10, -10};
    int16_t x2[8] = {32767, -32768, 16384, -16384, 30000, -30000, 1, -1};
    int16_t x3[8] = {32767, 32767, 4, 4, 3, 3, -32768, -32768};
    uint16_t x4[8] = {0xffff, 0x8000, 0x1234, 0x00ff, 0x0100, 0x0001, 0xffff, 0x4000};
    uint16_t x5[8] = {0xffff, 0x8000, 0x1000, 0x0100, 0x0100, 0xffff, 0x0002, 0x0004};
    int16_t x6[8] = {1, 2, -1, 2, 30000, 2, -30000, 2};
    int16_t x7[8] = {3, 4, 5, 6, 2, 10, 2, 10};
    memcpy(ctx->regs.x64.xmm[0], x0, sizeof(x0));
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[2], x2, sizeof(x2));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));
    memcpy(ctx->regs.x64.xmm[4], x4, sizeof(x4));
    memcpy(ctx->regs.x64.xmm[5], x5, sizeof(x5));
    memcpy(ctx->regs.x64.xmm[6], x6, sizeof(x6));
    memcpy(ctx->regs.x64.xmm[7], x7, sizeof(x7));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint16_t expected_pmullw[8] = {0x07d0, 0xf448, 0xea60, 0x15a0, 0xfffe, 0x0000, 0xcfcc, 0x3034};
    uint16_t expected_pmulhw[8] = {0x3fff, 0xc000, 0x0001, 0xffff, 0x0001, 0xfffe, 0xffff, 0x0000};
    uint16_t expected_pmulhuw[8] = {0xfffe, 0x4000, 0x0123, 0x0000, 0x0001, 0x0000, 0x0001, 0x0001};
    int32_t expected_pmaddwd[4] = {11, 7, 60020, -59980};
    ASSERT(memcmp(ctx->regs.x64.xmm[0], expected_pmullw, sizeof(expected_pmullw)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[2], expected_pmulhw, sizeof(expected_pmulhw)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[4], expected_pmulhuw, sizeof(expected_pmulhuw)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[6], expected_pmaddwd, sizeof(expected_pmaddwd)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_saturating_add_average_shuffle_blit_batch) {
    struct {
        uint8_t bytes[5];
        size_t len;
        int opcode;
    } cases[] = {
        {{0x66, 0x0f, 0xec, 0xc1, 0x00}, 4, HB_INS_PADDSB},
        {{0x66, 0x0f, 0xed, 0xc1, 0x00}, 4, HB_INS_PADDSW},
        {{0x66, 0x0f, 0xdc, 0xc1, 0x00}, 4, HB_INS_PADDUSB},
        {{0x66, 0x0f, 0xdd, 0xc1, 0x00}, 4, HB_INS_PADDUSW},
        {{0x66, 0x0f, 0xe0, 0xc1, 0x00}, 4, HB_INS_PAVGB},
        {{0x66, 0x0f, 0xe3, 0xc1, 0x00}, 4, HB_INS_PAVGW},
        {{0x66, 0x0f, 0x38, 0x00, 0xc1}, 5, HB_INS_PSHUFB},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].bytes, cases[i].len,
                             (uint64_t)(uintptr_t)cases[i].bytes, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    }

    uint8_t padd_code[] = {
        0x66, 0x0f, 0xdc, 0xc1, /* paddusb %xmm1, %xmm0 */
        0x66, 0x0f, 0xdd, 0xd3, /* paddusw %xmm3, %xmm2 */
        0x66, 0x0f, 0xec, 0xe5, /* paddsb  %xmm5, %xmm4 */
        0x66, 0x0f, 0xed, 0xf7  /* paddsw  %xmm7, %xmm6 */
    };
    uint64_t base = (uint64_t)(uintptr_t)padd_code;
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, padd_code, sizeof(padd_code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)padd_code, sizeof(padd_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint8_t u8a[16] = {250, 10, 0, 128, 255, 1, 100, 200, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t u8b[16] = {10, 20, 0, 128, 1, 255, 200, 55, 250, 249, 248, 247, 246, 245, 244, 243};
    uint16_t u16a[8] = {65000, 1, 1000, 65535, 0, 40000, 32768, 600};
    uint16_t u16b[8] = {1000, 65535, 2000, 1, 0, 40000, 32768, 60000};
    int8_t s8a[16] = {120, 100, -120, -100, 0, 60, -1, 127, -128, 50, -50, 10, 1, 2, 3, 4};
    int8_t s8b[16] = {10, 60, -20, -60, 0, 80, -1, 1, -1, -100, -100, 120, 127, -128, 3, -10};
    int16_t s16a[8] = {30000, 20000, -30000, -20000, 0, 32767, -32768, 100};
    int16_t s16b[8] = {10000, 20000, -10000, -20000, 0, 1, -1, -500};
    memcpy(ctx->regs.x64.xmm[0], u8a, sizeof(u8a));
    memcpy(ctx->regs.x64.xmm[1], u8b, sizeof(u8b));
    memcpy(ctx->regs.x64.xmm[2], u16a, sizeof(u16a));
    memcpy(ctx->regs.x64.xmm[3], u16b, sizeof(u16b));
    memcpy(ctx->regs.x64.xmm[4], s8a, sizeof(s8a));
    memcpy(ctx->regs.x64.xmm[5], s8b, sizeof(s8b));
    memcpy(ctx->regs.x64.xmm[6], s16a, sizeof(s16a));
    memcpy(ctx->regs.x64.xmm[7], s16b, sizeof(s16b));

    hb_exec_result_t run_out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &run_out) == HB_OK);
    ASSERT(run_out.result == HB_OK);

    uint8_t expected_paddusb[16] = {255, 30, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    uint16_t expected_paddusw[8] = {65535, 65535, 3000, 65535, 0, 65535, 65535, 60600};
    int8_t expected_paddsb[16] = {127, 127, -128, -128, 0, 127, -2, 127, -128, -50, -128, 127, 127, -126, 6, -6};
    int16_t expected_paddsw[8] = {32767, 32767, -32768, -32768, 0, 32767, -32768, -400};
    ASSERT(memcmp(ctx->regs.x64.xmm[0], expected_paddusb, sizeof(expected_paddusb)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[2], expected_paddusw, sizeof(expected_paddusw)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[4], expected_paddsb, sizeof(expected_paddsb)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[6], expected_paddsw, sizeof(expected_paddsw)) == 0);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);

    uint8_t avg_shuffle_code[] = {
        0x66, 0x0f, 0xe0, 0xc1,       /* pavgb  %xmm1, %xmm0 */
        0x66, 0x0f, 0xe3, 0xd3,       /* pavgw  %xmm3, %xmm2 */
        0x66, 0x0f, 0x38, 0x00, 0xe5  /* pshufb %xmm5, %xmm4 */
    };
    base = (uint64_t)(uintptr_t)avg_shuffle_code;
    dec = hb_decoder_create(HB_ARCH_X64, avg_shuffle_code, sizeof(avg_shuffle_code), base);
    func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)avg_shuffle_code, sizeof(avg_shuffle_code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint8_t avg_a[16] = {0, 1, 2, 3, 254, 255, 10, 11, 100, 101, 102, 103, 200, 201, 202, 203};
    uint8_t avg_b[16] = {1, 1, 3, 4, 255, 255, 11, 12, 101, 102, 103, 104, 201, 202, 203, 204};
    uint16_t avgw_a[8] = {0, 1, 2, 65534, 65535, 100, 101, 40000};
    uint16_t avgw_b[8] = {1, 1, 3, 65535, 65535, 101, 102, 40001};
    uint8_t shuf_src[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t shuf_mask[16] = {0x0f, 0x0e, 0x80, 0x00, 0x01, 0x12, 0x83, 0x07,
                             0x08, 0x09, 0x1a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    memcpy(ctx->regs.x64.xmm[0], avg_a, sizeof(avg_a));
    memcpy(ctx->regs.x64.xmm[1], avg_b, sizeof(avg_b));
    memcpy(ctx->regs.x64.xmm[2], avgw_a, sizeof(avgw_a));
    memcpy(ctx->regs.x64.xmm[3], avgw_b, sizeof(avgw_b));
    memcpy(ctx->regs.x64.xmm[4], shuf_src, sizeof(shuf_src));
    memcpy(ctx->regs.x64.xmm[5], shuf_mask, sizeof(shuf_mask));

    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &run_out) == HB_OK);
    ASSERT(run_out.result == HB_OK);

    uint8_t expected_pavgb[16] = {1, 1, 3, 4, 255, 255, 11, 12, 101, 102, 103, 104, 201, 202, 203, 204};
    uint16_t expected_pavgw[8] = {1, 1, 3, 65535, 65535, 101, 102, 40001};
    uint8_t expected_pshufb[16] = {15, 14, 0, 0, 1, 2, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    ASSERT(memcmp(ctx->regs.x64.xmm[0], expected_pavgb, sizeof(expected_pavgb)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[2], expected_pavgw, sizeof(expected_pavgw)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[4], expected_pshufb, sizeof(expected_pshufb)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_pshuf_immediate_family_notepadpp_vector_path) {
    struct {
        uint8_t prefix;
        int opcode;
    } cases[] = {
        {0x66, HB_INS_PSHUFD},
        {0xf2, HB_INS_PSHUFLW},
        {0xf3, HB_INS_PSHUFHW},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t dec_code[] = {cases[i].prefix, 0x0f, 0x70, 0xc1, 0x1b};
        hb_decoded_t d;
        ASSERT(hb_decode_x64(dec_code, sizeof(dec_code), (uint64_t)(uintptr_t)dec_code, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
        ASSERT(d.op3.is_imm && d.op3.imm == 0x1b);
    }

    uint8_t code[] = {
        0x66, 0x0f, 0x70, 0xc1, 0x1b, /* pshufd  $0x1b, %xmm1, %xmm0 */
        0xf2, 0x0f, 0x70, 0xd3, 0x1b, /* pshuflw $0x1b, %xmm3, %xmm2 */
        0xf3, 0x0f, 0x70, 0xe5, 0x1b  /* pshufhw $0x1b, %xmm5, %xmm4 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint8_t x1[16], x3[16], x5[16];
    for (int i = 0; i < 16; i++) {
        x1[i] = (uint8_t)i;
        x3[i] = (uint8_t)(0x20 + i);
        x5[i] = (uint8_t)(0x40 + i);
    }
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));
    memcpy(ctx->regs.x64.xmm[5], x5, sizeof(x5));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint8_t got[16];
    uint8_t expected_pshufd[16] = {
        0x0c, 0x0d, 0x0e, 0x0f, 0x08, 0x09, 0x0a, 0x0b,
        0x04, 0x05, 0x06, 0x07, 0x00, 0x01, 0x02, 0x03
    };
    uint8_t expected_pshuflw[16] = {
        0x26, 0x27, 0x24, 0x25, 0x22, 0x23, 0x20, 0x21,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
    };
    uint8_t expected_pshufhw[16] = {
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x4e, 0x4f, 0x4c, 0x4d, 0x4a, 0x4b, 0x48, 0x49
    };
    memcpy(got, ctx->regs.x64.xmm[0], sizeof(got));
    ASSERT(memcmp(got, expected_pshufd, sizeof(got)) == 0);
    memcpy(got, ctx->regs.x64.xmm[2], sizeof(got));
    ASSERT(memcmp(got, expected_pshuflw, sizeof(got)) == 0);
    memcpy(got, ctx->regs.x64.xmm[4], sizeof(got));
    ASSERT(memcmp(got, expected_pshufhw, sizeof(got)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_psrldq_notepadpp_shift_bytes) {
    uint8_t code[] = {0x66, 0x0f, 0x73, 0xd8, 0x08}; /* psrldq $8, %xmm0 */
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRLDQ);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_imm && d.op2.imm == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = 0x1122334455667788ULL;
    ctx->regs.x64.xmm[0][1] = 0x99aabbccddeeff00ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.xmm[0][0] == 0x99aabbccddeeff00ULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_packed_immediate_shift_family_smoke_helper_toolbar) {
    uint8_t code[] = {
        0x66, 0x0f, 0x72, 0xd0, 0x1f, /* psrld $31, %xmm0 */
        0x66, 0x0f, 0x72, 0xe1, 0x01, /* psrad $1, %xmm1 */
        0x66, 0x0f, 0x71, 0xf2, 0x01, /* psllw $1, %xmm2 */
        0x66, 0x0f, 0x71, 0xe3, 0x0f, /* psraw $15, %xmm3 */
        0x66, 0x0f, 0x72, 0xf4, 0x04, /* pslld $4, %xmm4 */
        0x66, 0x0f, 0x71, 0xd5, 0x01  /* psrlw $1, %xmm5 */
    };
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, 5, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRLD);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_imm && d.op2.imm == 31);

    ASSERT(hb_decode_x64(code + 5, 5, base + 5, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRAD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1);
    ASSERT(d.op2.is_imm && d.op2.imm == 1);

    ASSERT(hb_decode_x64(code + 10, 5, base + 10, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSLLW);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM2);

    ASSERT(hb_decode_x64(code + 15, 5, base + 15, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRAW);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM3);

    ASSERT(hb_decode_x64(code + 20, 5, base + 20, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSLLD);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM4);

    ASSERT(hb_decode_x64(code + 25, 5, base + 25, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRLW);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM5);

    uint8_t psrlq[] = {0x66, 0x0f, 0x73, 0xd0, 0x01};
    uint8_t pslldq[] = {0x66, 0x0f, 0x73, 0xf8, 0x01};
    ASSERT(hb_decode_x64(psrlq, sizeof(psrlq), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSRLQ);
    ASSERT(hb_decode_x64(pslldq, sizeof(pslldq), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PSLLDQ);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;

    uint32_t x0[4] = {0x80000000u, 0x7fffffffu, 0xffffffffu, 0x00000001u};
    uint32_t x1[4] = {0xfffffffeu, 0x00000004u, 0x80000000u, 0x00000001u};
    uint16_t x2[8] = {0x0001, 0x4000, 0x8000, 0xffff, 0x0002, 0x7fff, 0x1234, 0x0000};
    uint16_t x3[8] = {0xffff, 0xfffe, 0x0001, 0x4000, 0x7fff, 0x8000, 0x0000, 0x8001};
    uint32_t x4[4] = {0x00000001u, 0x10000000u, 0x80000000u, 0xffffffffu};
    uint16_t x5[8] = {0x8000, 0xffff, 0x0001, 0x0002, 0x0000, 0x7fff, 0xaaaa, 0x5555};
    memcpy(ctx->regs.x64.xmm[0], x0, sizeof(x0));
    memcpy(ctx->regs.x64.xmm[1], x1, sizeof(x1));
    memcpy(ctx->regs.x64.xmm[2], x2, sizeof(x2));
    memcpy(ctx->regs.x64.xmm[3], x3, sizeof(x3));
    memcpy(ctx->regs.x64.xmm[4], x4, sizeof(x4));
    memcpy(ctx->regs.x64.xmm[5], x5, sizeof(x5));

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);

    uint32_t expected_x0[4] = {1, 0, 1, 0};
    uint32_t expected_x1[4] = {0xffffffffu, 0x00000002u, 0xc0000000u, 0x00000000u};
    uint16_t expected_x2[8] = {0x0002, 0x8000, 0x0000, 0xfffe, 0x0004, 0xfffe, 0x2468, 0x0000};
    uint16_t expected_x3[8] = {0xffff, 0xffff, 0x0000, 0x0000, 0x0000, 0xffff, 0x0000, 0xffff};
    uint32_t expected_x4[4] = {0x00000010u, 0x00000000u, 0x00000000u, 0xfffffff0u};
    uint16_t expected_x5[8] = {0x4000, 0x7fff, 0x0000, 0x0001, 0x0000, 0x3fff, 0x5555, 0x2aaa};
    ASSERT(memcmp(ctx->regs.x64.xmm[0], expected_x0, sizeof(expected_x0)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[1], expected_x1, sizeof(expected_x1)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[2], expected_x2, sizeof(expected_x2)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[3], expected_x3, sizeof(expected_x3)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[4], expected_x4, sizeof(expected_x4)) == 0);
    ASSERT(memcmp(ctx->regs.x64.xmm[5], expected_x5, sizeof(expected_x5)) == 0);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_pcmpeqw_pmovmskb_notepadpp_vector_compare) {
    uint8_t code[] = {
        0x66, 0x0f, 0x75, 0xc8, /* pcmpeqw %xmm0, %xmm1 */
        0x66, 0x0f, 0xd7, 0xd1  /* pmovmskb %xmm1, %edx */
    };
    hb_decoded_t d;
    uint64_t base = (uint64_t)(uintptr_t)code;

    ASSERT(hb_decode_x64(code, 4, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPEQW);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM0);

    ASSERT(hb_decode_x64(code + 4, 4, base + 4, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PMOVMSKB);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_RDX);
    ASSERT(d.op1.size == 4);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);

    uint8_t pcmpeqb[] = {0x66, 0x0f, 0x74, 0xc2}; /* pcmpeqb %xmm2, %xmm0 */
    ASSERT(hb_decode_x64(pcmpeqb, sizeof(pcmpeqb), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPEQB);

    uint8_t pcmpeqd[] = {0x66, 0x0f, 0x76, 0xc2}; /* pcmpeqd %xmm2, %xmm0 */
    ASSERT(hb_decode_x64(pcmpeqd, sizeof(pcmpeqd), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_PCMPEQD);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rdx = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[1][0] = 0x4444333322221111ULL;
    ctx->regs.x64.xmm[1][1] = 0x8888777766665555ULL;
    ctx->regs.x64.xmm[0][0] = 0x4440333322201111ULL;
    ctx->regs.x64.xmm[0][1] = 0x8880777766605555ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT(ctx->regs.x64.rdx == 0x3333);
    ASSERT(ctx->regs.x64.xmm[1][0] == 0x0000ffff0000ffffULL);
    ASSERT(ctx->regs.x64.xmm[1][1] == 0x0000ffff0000ffffULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_cvtsi2ss_mulss_comiss_calc_cluster) {
    struct {
        uint8_t code[32];
        uint8_t pad[0x1c];
        float multiplier;
    } block __attribute__((aligned(16)));
    memset(&block, 0xcc, sizeof(block));

    uint64_t base = (uint64_t)(uintptr_t)block.code;
    block.code[0] = 0xf3;  /* cvtsi2ss %rax, %xmm0 */
    block.code[1] = 0x48;
    block.code[2] = 0x0f;
    block.code[3] = 0x2a;
    block.code[4] = 0xc0;
    block.code[5] = 0x0f;  /* movaps %xmm0, %xmm2 */
    block.code[6] = 0x28;
    block.code[7] = 0xd0;
    block.code[8] = 0xf3;  /* mulss 0x1c(%rcx), %xmm2 */
    block.code[9] = 0x0f;
    block.code[10] = 0x59;
    block.code[11] = 0x51;
    block.code[12] = 0x1c;
    block.code[13] = 0x0f; /* comiss %xmm1, %xmm2 */
    block.code[14] = 0x2f;
    block.code[15] = 0xd1;
    block.code[16] = 0xf3; /* cvttss2si %xmm2, %rax */
    block.code[17] = 0x48;
    block.code[18] = 0x0f;
    block.code[19] = 0x2c;
    block.code[20] = 0xc2;
    block.multiplier = 2.0f;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(block.code, 5, base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CVTSI2SS);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_RAX);
    ASSERT(d.op2.size == 8);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, block.code, 21, base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)&block, sizeof(block),
                         HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 17;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)block.pad;
    ctx->regs.x64.xmm[0][0] = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;
    ctx->regs.x64.xmm[1][0] = test_float_bits(30.0f);
    ctx->regs.x64.xmm[2][0] = 0x1111111122222222ULL;
    ctx->regs.x64.xmm[2][1] = 0x3333333344444444ULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.xmm[2][0] == test_float_bits(34.0f));
    ASSERT(ctx->regs.x64.rax == 34);
    ASSERT(ctx->flags.cf == false);
    ASSERT(ctx->flags.zf == false);
    ASSERT(ctx->flags.pf == false);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_divss_notepadpp_scalar_float) {
    uint8_t code[] = {0xf3, 0x0f, 0x5e, 0xc1}; /* divss %xmm1, %xmm0 */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_DIVSS);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[0][0] = test_float_bits(9.0f) | 0xaaaaaaaa00000000ULL;
    ctx->regs.x64.xmm[0][1] = 0xbbbbbbbbbbbbbbbbULL;
    ctx->regs.x64.xmm[1][0] = test_float_bits(3.0f);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.xmm[0][0] == test_float_bits(3.0f));
    ASSERT((ctx->regs.x64.xmm[0][0] >> 32) == 0xaaaaaaaaULL);
    ASSERT(ctx->regs.x64.xmm[0][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_sqrt_family_0f51) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
        size_t src_size;
    } cases[] = {
        {{0x0f, 0x51, 0xc1, 0x00}, 3, HB_INS_SQRTPS, 16},
        {{0x66, 0x0f, 0x51, 0xc1}, 4, HB_INS_SQRTPD, 16},
        {{0xf3, 0x0f, 0x51, 0xc1}, 4, HB_INS_SQRTSS, 4},
        {{0xf2, 0x0f, 0x51, 0xc1}, 4, HB_INS_SQRTSD, 8},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].code, cases[i].len, 0x140001000ULL, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
        ASSERT(d.op2.size == 16);

        hb_decoded_t mem;
        uint8_t mem_code[8];
        memcpy(mem_code, cases[i].code, cases[i].len);
        mem_code[cases[i].len - 1] = 0x09; /* sqrt* xmm1, [rcx] */
        ASSERT(hb_decode_x64(mem_code, cases[i].len, 0x140001000ULL, &mem) == HB_OK);
        ASSERT((int)mem.opcode == cases[i].opcode);
        ASSERT(mem.op1.is_reg && mem.op1.reg == HB_REG_XMM1);
        ASSERT(mem.op2.is_mem && mem.op2.mem.base == HB_REG_RCX);
        ASSERT(mem.op2.size == cases[i].src_size);
    }
    tests_passed++;
}

TEST(interp_x64_sqrtss_unityplayer_scalar_float) {
    uint8_t code[] = {0xf3, 0x0f, 0x51, 0xce}; /* sqrtss %xmm6, %xmm1 */
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SQRTSS);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM1);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM6);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.xmm[1][0] = test_float_bits(1.0f) | 0xaaaaaaaa00000000ULL;
    ctx->regs.x64.xmm[1][1] = 0xbbbbbbbbbbbbbbbbULL;
    ctx->regs.x64.xmm[6][0] = test_float_bits(16.0f);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.xmm[1][0] == test_float_bits(4.0f));
    ASSERT((ctx->regs.x64.xmm[1][0] >> 32) == 0xaaaaaaaaULL);
    ASSERT(ctx->regs.x64.xmm[1][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_rsqrt_rcp_family_0f52_0f53) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
        size_t mem_size;
    } cases[] = {
        {{0x0f, 0x52, 0xc1, 0x00}, 3, HB_INS_RSQRTPS, 16},
        {{0xf3, 0x0f, 0x52, 0xc1}, 4, HB_INS_RSQRTSS, 4},
        {{0x0f, 0x53, 0xc1, 0x00}, 3, HB_INS_RCPPS, 16},
        {{0xf3, 0x0f, 0x53, 0xc1}, 4, HB_INS_RCPSS, 4},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].code, cases[i].len, 0x140001000ULL, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
        ASSERT(d.op2.size == 16);

        hb_decoded_t mem;
        uint8_t mem_code[8];
        memcpy(mem_code, cases[i].code, cases[i].len);
        mem_code[cases[i].len - 1] = 0x09; /* reciprocal-estimate xmm1, [rcx] */
        ASSERT(hb_decode_x64(mem_code, cases[i].len, 0x140001000ULL, &mem) == HB_OK);
        ASSERT((int)mem.opcode == cases[i].opcode);
        ASSERT(mem.op1.is_reg && mem.op1.reg == HB_REG_XMM1);
        ASSERT(mem.op2.is_mem && mem.op2.mem.base == HB_REG_RCX);
        ASSERT(mem.op2.size == cases[i].mem_size);
    }
    tests_passed++;
}

TEST(interp_x64_rsqrtps_rcpss_reciprocal_estimate_family) {
    uint8_t code[] = {
        0x0f, 0x52, 0xc1,       /* rsqrtps %xmm1, %xmm0 */
        0xf3, 0x0f, 0x53, 0xd3  /* rcpss %xmm3, %xmm2 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t inputs[4] = {
        test_float_bits(1.0f),
        test_float_bits(4.0f),
        test_float_bits(16.0f),
        test_float_bits(64.0f),
    };
    memcpy(ctx->regs.x64.xmm[1], inputs, sizeof(inputs));
    ctx->regs.x64.xmm[2][0] = test_float_bits(8.0f) | 0xaaaaaaaa00000000ULL;
    ctx->regs.x64.xmm[2][1] = 0xbbbbbbbbbbbbbbbbULL;
    ctx->regs.x64.xmm[3][0] = test_float_bits(4.0f);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t results[4];
    memcpy(results, ctx->regs.x64.xmm[0], sizeof(results));
    ASSERT(results[0] == test_float_bits(1.0f));
    ASSERT(results[1] == test_float_bits(0.5f));
    ASSERT(results[2] == test_float_bits(0.25f));
    ASSERT(results[3] == test_float_bits(0.125f));
    ASSERT((uint32_t)ctx->regs.x64.xmm[2][0] == test_float_bits(0.25f));
    ASSERT((ctx->regs.x64.xmm[2][0] >> 32) == 0xaaaaaaaaULL);
    ASSERT(ctx->regs.x64.xmm[2][1] == 0xbbbbbbbbbbbbbbbbULL);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_mul_div_packed_fp_family_0f59_0f5e) {
    struct {
        uint8_t code[4];
        size_t len;
        int opcode;
    } cases[] = {
        {{0x0f, 0x59, 0xc1, 0x00}, 3, HB_INS_MULPS},
        {{0x66, 0x0f, 0x59, 0xc1}, 4, HB_INS_MULPD},
        {{0x0f, 0x5e, 0xc1, 0x00}, 3, HB_INS_DIVPS},
        {{0x66, 0x0f, 0x5e, 0xc1}, 4, HB_INS_DIVPD},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hb_decoded_t d;
        ASSERT(hb_decode_x64(cases[i].code, cases[i].len, 0x140001000ULL, &d) == HB_OK);
        ASSERT((int)d.opcode == cases[i].opcode);
        ASSERT(d.len == cases[i].len);
        ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
        ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
        ASSERT(d.op2.size == 16);
    }
    tests_passed++;
}

TEST(interp_x64_divps_mulpd_packed_fp_family) {
    uint8_t code[] = {
        0x0f, 0x5e, 0xc8,             /* divps %xmm0, %xmm1 */
        0x66, 0x0f, 0x59, 0xd3        /* mulpd %xmm3, %xmm2 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t divisors[4] = {
        test_float_bits(2.0f), test_float_bits(3.0f),
        test_float_bits(5.0f), test_float_bits(4.0f),
    };
    uint32_t dividends[4] = {
        test_float_bits(8.0f), test_float_bits(9.0f),
        test_float_bits(10.0f), test_float_bits(12.0f),
    };
    memcpy(ctx->regs.x64.xmm[0], divisors, sizeof(divisors));
    memcpy(ctx->regs.x64.xmm[1], dividends, sizeof(dividends));
    ctx->regs.x64.xmm[2][0] = test_double_bits(2.0);
    ctx->regs.x64.xmm[2][1] = test_double_bits(3.0);
    ctx->regs.x64.xmm[3][0] = test_double_bits(4.0);
    ctx->regs.x64.xmm[3][1] = test_double_bits(5.0);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t div_results[4];
    memcpy(div_results, ctx->regs.x64.xmm[1], sizeof(div_results));
    ASSERT(div_results[0] == test_float_bits(4.0f));
    ASSERT(div_results[1] == test_float_bits(3.0f));
    ASSERT(div_results[2] == test_float_bits(2.0f));
    ASSERT(div_results[3] == test_float_bits(3.0f));
    ASSERT(ctx->regs.x64.xmm[2][0] == test_double_bits(8.0));
    ASSERT(ctx->regs.x64.xmm[2][1] == test_double_bits(15.0));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_x64_shufps_shufpd_family_0fc6) {
    hb_decoded_t d;
    uint8_t shufps[] = {0x0f, 0xc6, 0xc1, 0xaa};
    uint8_t shufpd[] = {0x66, 0x0f, 0xc6, 0xc1, 0x01};

    ASSERT(hb_decode_x64(shufps, sizeof(shufps), 0x140001000ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHUFPS);
    ASSERT(d.len == 4);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    ASSERT(d.op3.is_imm && d.op3.imm == 0xaa);

    ASSERT(hb_decode_x64(shufpd, sizeof(shufpd), 0x140001000ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_SHUFPD);
    ASSERT(d.len == 5);
    ASSERT(d.op1.is_reg && d.op1.reg == HB_REG_XMM0);
    ASSERT(d.op2.is_reg && d.op2.reg == HB_REG_XMM1);
    ASSERT(d.op3.is_imm && d.op3.imm == 0x01);
    tests_passed++;
}

TEST(interp_x64_shufps_shufpd_family) {
    uint8_t code[] = {
        0x0f, 0xc6, 0xc1, 0xaa,       /* shufps $0xaa, %xmm1, %xmm0 */
        0x66, 0x0f, 0xc6, 0xd3, 0x01  /* shufpd $0x01, %xmm3, %xmm2 */
    };
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    uint32_t lhs_ps[4] = {
        test_float_bits(1.0f), test_float_bits(2.0f),
        test_float_bits(3.0f), test_float_bits(4.0f),
    };
    uint32_t rhs_ps[4] = {
        test_float_bits(10.0f), test_float_bits(20.0f),
        test_float_bits(30.0f), test_float_bits(40.0f),
    };
    memcpy(ctx->regs.x64.xmm[0], lhs_ps, sizeof(lhs_ps));
    memcpy(ctx->regs.x64.xmm[1], rhs_ps, sizeof(rhs_ps));
    ctx->regs.x64.xmm[2][0] = test_double_bits(1.0);
    ctx->regs.x64.xmm[2][1] = test_double_bits(2.0);
    ctx->regs.x64.xmm[3][0] = test_double_bits(10.0);
    ctx->regs.x64.xmm[3][1] = test_double_bits(20.0);

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    uint32_t shufps_result[4];
    memcpy(shufps_result, ctx->regs.x64.xmm[0], sizeof(shufps_result));
    ASSERT(shufps_result[0] == test_float_bits(3.0f));
    ASSERT(shufps_result[1] == test_float_bits(3.0f));
    ASSERT(shufps_result[2] == test_float_bits(30.0f));
    ASSERT(shufps_result[3] == test_float_bits(30.0f));
    ASSERT(ctx->regs.x64.xmm[2][0] == test_double_bits(2.0));
    ASSERT(ctx->regs.x64.xmm[2][1] == test_double_bits(10.0));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(decode_cpuid_opcode_0fa2) {
    uint8_t code[] = {0x0f, 0xa2};
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1403e5d49ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_CPUID);
    ASSERT(d.len == 2);
    tests_passed++;
}

TEST(decode_x64_truncated_sib_fails_cleanly) {
    uint8_t code[] = {0x8b, 0x04}; /* missing required SIB byte */
    hb_decoded_t d;
    memset(&d, 0, sizeof(d));
    hb_result_t r = hb_decode_x64(code, sizeof(code), 0x1000, &d);
    ASSERT(r == HB_ERR_DECODE_FAILED);
    ASSERT(d.len > 0);
    ASSERT(d.len <= sizeof(code));
    tests_passed++;
}

TEST(decode_xgetbv_opcode_0f01d0) {
    uint8_t code[] = {0x0f, 0x01, 0xd0};
    hb_decoded_t d;
    ASSERT(hb_decode_x64(code, sizeof(code), 0x1403e5e81ULL, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_XGETBV);
    ASSERT(d.len == 3);
    tests_passed++;
}

TEST(interp_x64_cpuid_vendor_and_leaf1) {
    uint8_t code[] = {0x0f, 0xa2, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0xa2};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rbx = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.rcx = 0;
    ctx->regs.x64.rdx = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x000306a9U);
    ASSERT(((uint32_t)ctx->regs.x64.rcx & (1u << 27)) == 0); /* OSXSAVE: not advertised yet */
    ASSERT(((uint32_t)ctx->regs.x64.rcx & (1u << 28)) == 0); /* AVX: not advertised yet */
    ASSERT((uint32_t)ctx->regs.x64.rdx & (1u << 26)); /* SSE2 */

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_xgetbv_leaf0) {
    uint8_t code[] = {0x31, 0xc9, 0x0f, 0x01, 0xd0};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.rcx = 0x1111111111111111ULL;
    ctx->regs.x64.rdx = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x3U);
    ASSERT((uint32_t)ctx->regs.x64.rdx == 0x0U);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_xgetbv_leaf0) {
    uint8_t code[] = {0x31, 0xc9, 0x0f, 0x01, 0xd0};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.rcx = 0x1111111111111111ULL;
    ctx->regs.x64.rdx = 0xbbbbbbbbbbbbbbbbULL;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x3U);
    ASSERT((uint32_t)ctx->regs.x64.rdx == 0x0U);

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(jit_x64_cpuid_vendor_and_leaf1) {
    uint8_t code[] = {0x0f, 0xa2, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0xa2};
    uint64_t base = (uint64_t)(uintptr_t)code;

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, base, sizeof(code), HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rax = 0;
    ctx->regs.x64.rbx = 0xaaaaaaaaaaaaaaaaULL;
    ctx->regs.x64.rcx = 0;
    ctx->regs.x64.rdx = 0;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_JIT, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    ASSERT((uint32_t)ctx->regs.x64.rax == 0x000306a9U);
    ASSERT(((uint32_t)ctx->regs.x64.rcx & (1u << 27)) == 0); /* OSXSAVE: not advertised yet */
    ASSERT(((uint32_t)ctx->regs.x64.rcx & (1u << 28)) == 0); /* AVX: not advertised yet */
    ASSERT((uint32_t)ctx->regs.x64.rdx & (1u << 26)); /* SSE2 */

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

TEST(interp_x64_control_state_setjmp_family) {
    uint8_t code[] = {
        0x0f, 0xae, 0x59, 0x58, /* stmxcsr 0x58(%rcx) */
        0xd9, 0x79, 0x5c,       /* fnstcw  0x5c(%rcx) */
        0x0f, 0xae, 0x51, 0x58, /* ldmxcsr 0x58(%rcx) */
        0xd9, 0x69, 0x5c,       /* fldcw   0x5c(%rcx) */
        0x9b,                   /* fwait */
        0x0f, 0xae, 0xe8,       /* lfence */
        0x0f, 0xae, 0xf0,       /* mfence */
        0x0f, 0xae, 0xf8        /* sfence */
    };
    uint8_t frame[0x70];
    uint64_t base = (uint64_t)(uintptr_t)code;
    hb_decoded_t d;
    uint32_t mxcsr;
    uint16_t control_word;

    memset(frame, 0xaa, sizeof(frame));
    ASSERT(hb_decode_x64(code, sizeof(code), base, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 4 && d.op1.mem.base == HB_REG_RCX && d.op1.mem.disp == 0x58);
    ASSERT(d.op2.is_imm && d.op2.size == 4 && d.op2.imm == 0x1f80);

    ASSERT(hb_decode_x64(code + 4, sizeof(code) - 4, base + 4, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_MOV);
    ASSERT(d.op1.is_mem && d.op1.size == 2 && d.op1.mem.base == HB_REG_RCX && d.op1.mem.disp == 0x5c);
    ASSERT(d.op2.is_imm && d.op2.size == 2 && d.op2.imm == 0x037f);

    ASSERT(hb_decode_x64(code + 7, sizeof(code) - 7, base + 7, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 4);
    ASSERT(hb_decode_x64(code + 11, sizeof(code) - 11, base + 11, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 3);
    ASSERT(hb_decode_x64(code + 14, sizeof(code) - 14, base + 14, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 1);
    ASSERT(hb_decode_x64(code + 15, sizeof(code) - 15, base + 15, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 3);
    ASSERT(hb_decode_x64(code + 18, sizeof(code) - 18, base + 18, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 3);
    ASSERT(hb_decode_x64(code + 21, sizeof(code) - 21, base + 21, &d) == HB_OK);
    ASSERT(d.opcode == HB_INS_NOP && d.len == 3);

    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, sizeof(code), base);
    hb_ir_func_t* func = NULL;
    ASSERT(dec != NULL);
    ASSERT(hb_lift_func_x64(dec, &func) == HB_OK);
    hb_decoder_destroy(dec);
    ASSERT(func != NULL);

    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    ASSERT(ctx != NULL);
    ctx->memory = hb_memory_create(0);
    ASSERT(ctx->memory != NULL);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)code, sizeof(code),
                         HB_PERM_READ | HB_PERM_EXEC) == HB_OK);
    ASSERT(hb_memory_map(ctx->memory, (hb_gva_t)(uintptr_t)frame, sizeof(frame),
                         HB_PERM_READ | HB_PERM_WRITE) == HB_OK);
    ctx->pc = base;
    ctx->regs.x64.rcx = (uint64_t)(uintptr_t)frame;

    hb_exec_result_t out;
    ASSERT(hb_runtime_run(ctx, func, HB_BACKEND_INTERP, &out) == HB_OK);
    ASSERT(out.result == HB_OK);
    memcpy(&mxcsr, frame + 0x58, sizeof(mxcsr));
    memcpy(&control_word, frame + 0x5c, sizeof(control_word));
    ASSERT(mxcsr == 0x1f80);
    ASSERT(control_word == 0x037f);
    ASSERT(frame[0x57] == 0xaa);
    ASSERT(frame[0x5e] == 0xaa);
    ASSERT(ctx->pc == base + sizeof(code));

    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    tests_passed++;
}

static hb_ir_func_t* phase2_make_hot_loop_func(void) {
    hb_ir_func_t* func = hb_ir_func_create(0x1000, 0);
    if (!func) return NULL;

    hb_ir_block_t* blkA = hb_ir_block_create(0, 0x1000);
    hb_ir_block_t* blkB = hb_ir_block_create(1, 0x1010);
    hb_ir_block_t* blkD = hb_ir_block_create(2, 0x1030);
    if (!blkA || !blkB || !blkD) {
        hb_ir_func_destroy(func);
        return NULL;
    }
    hb_ir_cfg_add_block(func->cfg, blkA);
    hb_ir_cfg_add_block(func->cfg, blkB);
    hb_ir_cfg_add_block(func->cfg, blkD);
    func->cfg->entry = blkA;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    if (!b) {
        hb_ir_func_destroy(func);
        return NULL;
    }

    hb_ir_builder_set_block(b, blkA);
    hb_ir_instr_t* cmpA = hb_ir_emit_cmp(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(0, HB_SIZE_64));
    cmpA->guest_addr = 0x1000; cmpA->guest_len = 4;
    hb_ir_instr_t* jccA = hb_ir_emit_jcc(b, HB_CC_E, 0x1030);
    jccA->guest_addr = 0x1004; jccA->guest_len = 0x0c;

    hb_ir_builder_set_block(b, blkB);
    hb_ir_instr_t* subB = hb_ir_emit_binop(b, HB_IR_SUB, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                           hb_ir_reg(HB_REG_RAX, HB_SIZE_64), hb_ir_imm(1, HB_SIZE_64));
    subB->guest_addr = 0x1010; subB->guest_len = 4;
    hb_ir_instr_t* jmpB = hb_ir_emit_jmp(b, 0x1000);
    jmpB->guest_addr = 0x1014; jmpB->guest_len = 2;

    hb_ir_builder_destroy(b);
    return func;
}

static hb_ir_func_t* phase2_make_cached_mov_func(size_t ops_per_block) {
    hb_ir_func_t* func = hb_ir_func_create(0x2000, 0);
    if (!func) return NULL;
    hb_ir_block_t* block = hb_ir_block_create(0, 0x2000);
    if (!block) {
        hb_ir_func_destroy(func);
        return NULL;
    }
    hb_ir_cfg_add_block(func->cfg, block);
    func->cfg->entry = block;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    if (!b) {
        hb_ir_func_destroy(func);
        return NULL;
    }
    hb_ir_builder_set_block(b, block);
    for (size_t i = 0; i < ops_per_block; i++) {
        hb_ir_instr_t* mov = hb_ir_emit_mov(b, hb_ir_reg(HB_REG_RAX, HB_SIZE_64),
                                            hb_ir_imm((uint64_t)i, HB_SIZE_64));
        mov->guest_addr = 0x2000 + i * 4;
        mov->guest_len = 4;
    }
    hb_ir_builder_destroy(b);
    return func;
}

static int phase2_run_one_backend(const hb_ir_func_t* func, hb_backend_t backend,
                                  uint64_t iterations, uint64_t* elapsed_ns,
                                  hb_exec_result_t* out) {
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, backend);
    if (!ctx) return 1;
    ctx->pc = 0x1000;
    ctx->regs.x64.rax = iterations;
    ctx->block_limit = iterations * 3 + 32;
    ctx->step_limit = iterations * 8 + 64;

    uint64_t start = hb_test_now_ns();
    hb_result_t r = hb_runtime_run(ctx, func, backend, out);
    *elapsed_ns = hb_test_now_ns() - start;
    int failed = (r != HB_OK || out->result != HB_OK || ctx->regs.x64.rax != 0);
    if (failed)
        fprintf(stderr, "phase2 bench backend=%d failed r=%d out=%d faulted=%d reason=%s rax=%llu pc=0x%llx blocks=%llu steps=%llu\n",
                backend, r, out->result, out->faulted, out->fault_reason ? out->fault_reason : "?",
                (unsigned long long)ctx->regs.x64.rax, (unsigned long long)ctx->pc,
                (unsigned long long)out->blocks_executed, (unsigned long long)out->steps_executed);
    hb_context_destroy(ctx);
    return failed;
}

static int phase2_run_cached_block_bench(uint64_t* interp_ns, uint64_t* jit_ns,
                                         uint64_t* ops_out, uint64_t* rounds_out) {
    const size_t ops_per_block = 96;
    const uint64_t rounds = 10000;
    hb_ir_func_t* func = phase2_make_cached_mov_func(ops_per_block);
    if (!func) return 1;

    hb_context_t* interp_ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_INTERP);
    hb_context_t* jit_ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_interpreter_t* interp = interp_ctx ? hb_interpreter_create(interp_ctx) : NULL;
    hb_jit_runtime_t* jit = jit_ctx ? hb_jit_runtime_create(jit_ctx) : NULL;
    hb_exec_result_t out;
    if (!interp_ctx || !jit_ctx || !interp || !jit) {
        if (interp) hb_interpreter_destroy(interp);
        if (jit) hb_jit_runtime_destroy(jit);
        if (interp_ctx) hb_context_destroy(interp_ctx);
        if (jit_ctx) hb_context_destroy(jit_ctx);
        hb_ir_func_destroy(func);
        return 1;
    }

    jit_ctx->pc = 0x2000;
    if (hb_jit_runtime_run(jit, func, &out) != HB_OK || out.result != HB_OK) {
        hb_interpreter_destroy(interp);
        hb_jit_runtime_destroy(jit);
        hb_context_destroy(interp_ctx);
        hb_context_destroy(jit_ctx);
        hb_ir_func_destroy(func);
        return 1;
    }

    uint64_t start = hb_test_now_ns();
    for (uint64_t i = 0; i < rounds; i++) {
        interp_ctx->pc = 0x2000;
        if (hb_interpreter_run(interp, func, &out) != HB_OK || out.result != HB_OK) {
            hb_interpreter_destroy(interp);
            hb_jit_runtime_destroy(jit);
            hb_context_destroy(interp_ctx);
            hb_context_destroy(jit_ctx);
            hb_ir_func_destroy(func);
            return 1;
        }
    }
    *interp_ns = hb_test_now_ns() - start;

    start = hb_test_now_ns();
    for (uint64_t i = 0; i < rounds; i++) {
        jit_ctx->pc = 0x2000;
        if (hb_jit_runtime_run(jit, func, &out) != HB_OK || out.result != HB_OK) {
            hb_interpreter_destroy(interp);
            hb_jit_runtime_destroy(jit);
            hb_context_destroy(interp_ctx);
            hb_context_destroy(jit_ctx);
            hb_ir_func_destroy(func);
            return 1;
        }
    }
    *jit_ns = hb_test_now_ns() - start;
    *ops_out = rounds * ops_per_block;
    *rounds_out = rounds;

    hb_interpreter_destroy(interp);
    hb_jit_runtime_destroy(jit);
    hb_context_destroy(interp_ctx);
    hb_context_destroy(jit_ctx);
    hb_ir_func_destroy(func);
    return 0;
}

static int run_phase2_bench(void) {
    const uint64_t iterations = 50000;
    hb_ir_func_t* func = phase2_make_hot_loop_func();
    hb_exec_result_t interp_out, jit_out;
    uint64_t interp_ns = 0, jit_ns = 0;
    uint64_t cached_interp_ns = 0, cached_jit_ns = 0, cached_ops = 0, cached_rounds = 0;
    if (!func) return 1;

    if (phase2_run_one_backend(func, HB_BACKEND_INTERP, iterations, &interp_ns, &interp_out)) {
        hb_ir_func_destroy(func);
        return 1;
    }
    if (phase2_run_one_backend(func, HB_BACKEND_JIT, iterations, &jit_ns, &jit_out)) {
        hb_ir_func_destroy(func);
        return 1;
    }

    double speedup = jit_ns ? (double)interp_ns / (double)jit_ns : 0.0;
    if (phase2_run_cached_block_bench(&cached_interp_ns, &cached_jit_ns, &cached_ops, &cached_rounds)) {
        hb_ir_func_destroy(func);
        return 1;
    }
    double cached_speedup = cached_jit_ns ? (double)cached_interp_ns / (double)cached_jit_ns : 0.0;
    printf("{\"phase\":\"phase2_hot_loop\",\"iterations\":%llu,"
           "\"interpreter_ns\":%llu,\"jit_ns\":%llu,\"speedup\":%.2f,"
           "\"interp_blocks\":%llu,\"jit_blocks\":%llu,"
           "\"per_block_compile\":\"on-demand\",\"block_chaining\":\"pc-target loop in hb_jit_runtime_run\","
           "\"cached_block_ops\":%llu,\"cached_block_rounds\":%llu,"
           "\"cached_block_interpreter_ns\":%llu,\"cached_block_jit_ns\":%llu,"
           "\"cached_block_speedup\":%.2f}\n",
           (unsigned long long)iterations,
           (unsigned long long)interp_ns,
           (unsigned long long)jit_ns,
           speedup,
           (unsigned long long)interp_out.blocks_executed,
           (unsigned long long)jit_out.blocks_executed,
           (unsigned long long)cached_ops,
           (unsigned long long)cached_rounds,
           (unsigned long long)cached_interp_ns,
           (unsigned long long)cached_jit_ns,
           cached_speedup);
    hb_ir_func_destroy(func);
    return cached_speedup >= 10.0 ? 0 : 2;
}

/* --- Entry point --- */

int main(int argc, char** argv) {
    printf("HyperBridge C test runner\n");
    if (argc == 2 && strcmp(argv[1], "--phase2-bench") == 0)
        return run_phase2_bench();
    if (argc == 3 && strcmp(argv[1], "--fast-family") == 0) {
        if (!strcmp(argv[2], "rep_movs") || !strcmp(argv[2], "string_ops")) {
            printf("rep_movs_enter\n");
            printf("df=0\n");
            printf("rcx=16\n");
            test_decode_rep_movsq_icon_memcpy();
            test_decode_rep_movsb_icon_memcpy();
            test_decode_x86_string_ops_family();
            test_interp_x64_rep_movsq_icon_memcpy_forward();
            test_interp_x86_rep_stosd_signal_stack_clear();
            printf("write_ok\n");
            printf("rep_movs_exit\n");
            printf("rep_movs_enter\n");
            printf("df=1\n");
            printf("rcx=16\n");
            test_interp_x64_rep_movsb_direction_flag_backward();
            test_interp_x86_rep_movsb_direction_flag_backward();
            printf("write_ok\n");
            printf("rep_movs_exit\n");
            printf("cmps_enter\n");
            test_decode_repe_cmpsb_string_scan();
            test_interp_x64_repe_cmpsb_stops_on_mismatch();
            test_interp_x64_repne_cmpsb_stops_on_match();
            test_interp_x64_cmpsw_direction_flag_single_step();
            printf("cmps_exit\n");
            printf("lods_enter\n");
            test_decode_lodsq_string_load();
            test_interp_x64_lodsd_zero_extends_eax();
            test_interp_x64_rep_lodsb_direction_flag_backward();
            printf("lods_exit\n");
            printf("%d passed, %d failed\n", tests_passed, tests_failed);
            return tests_failed ? 1 : 0;
        }
        if (!strcmp(argv[2], "x87")) {
            test_x86_context_x87_defaults();
            test_x87_fldcw_fnstcw_fistp_rounding_modes();
            test_x87_environment_control_state_helpers();
            test_decode_x86_x87_environment_control_family();
            test_decode_x64_x87_environment_control_family();
            test_interp_x64_x87_environment_control_family();
            test_interp_x86_x87_load_store_control_conversion_core();
            test_interp_x86_x87_memory_arithmetic_compare_core();
            test_interp_x86_x87_stack_register_pop_core();
            test_interp_x86_x87_environment_control_family();
            printf("%d passed, %d failed\n", tests_passed, tests_failed);
            return tests_failed ? 1 : 0;
        }
        if (!strcmp(argv[2], "phase1_core")) {
            printf("phase1_core_enter\n");
            printf("flags_oracle_enter\n");
            test_phase1_x64_arith_logic_shift_flags_oracle_fuzzer();
            printf("flags_oracle_exit\n");
            printf("lazy_flags_enter\n");
            test_diff_lazy_flags_jcc_fuzzer();
            test_diff_lazy_flags_setcc_cmovcc_fuzzer();
            test_decode_x64_x86_loop_jrcxz_family();
            test_interp_x64_loop_jrcxz_family();
            test_jit_x64_loop_jrcxz_family();
            printf("lazy_flags_exit\n");
            printf("x87_enter\n");
            test_decode_x64_x87_environment_control_family();
            test_interp_x64_x87_environment_control_family();
            printf("x87_exit\n");
            printf("sse_enter\n");
            test_interp_x64_movaps_xmm_store();
            test_interp_x64_addpd_subpd_notepadpp_callback_block();
            test_interp_x64_packed_immediate_shift_family_smoke_helper_toolbar();
            printf("sse_exit\n");
            printf("atomics_enter\n");
            test_decode_xadd_lock_r32_calc_atomic();
            test_interp_x64_lock_xadd_r32_memory();
            test_interp_x64_cmpxchg8b_cmpxchg16b_family();
            printf("atomics_exit\n");
            printf("phase1_core_exit\n");
            printf("%d passed, %d failed\n", tests_passed, tests_failed);
            return tests_failed ? 1 : 0;
        }
        fprintf(stderr, "unknown fast family: %s\n", argv[2]);
        return 2;
    }
    test_decode_mov_reg_reg();
    test_decode_mov_r8_mem16_operand_override();
    test_decode_x64_operand16_immediate_lengths();
    test_decode_x64_mov_moffs_family();
    test_decode_x64_xchg_accumulator_opcode_family();
    test_decode_x64_segment_mov_leave_family();
    test_decode_x64_port_interrupt_flag_family();
    test_decode_x64_0f_system_segment_family();
    test_decode_x64_x87_pushf_rotate_family();
    test_decode_x64_prefix_pop_hlt_family();
    test_decode_x64_0f_unsupported_family_lengths();
    test_decode_x64_0f38_0f3a_vector_family_lengths();
    test_decode_x64_vex_evex_vector_family_lengths();
    test_decode_cpuid_opcode_0fa2();
    test_decode_x64_truncated_sib_fails_cleanly();
    test_decode_xgetbv_opcode_0f01d0();
    test_interp_x64_cpuid_vendor_and_leaf1();
    test_jit_x64_cpuid_vendor_and_leaf1();
    test_interp_x64_xgetbv_leaf0();
    test_jit_x64_xgetbv_leaf0();
    test_interp_x64_control_state_setjmp_family();
    test_decode_add_imm();
    test_decode_x64_accumulator_imm_logic_family();
    test_interp_x64_and_eax_imm32_calc_callback();
    test_interp_x64_linear_block_advances_pc();
    test_memory_special_write_unmapped_live_range();
    test_memory_cross_region_write_read_span();
    test_memory_private_guest_low_va_backing();
    test_memory_guest32_window_direct_mapping();
    test_memory_guest32_host_mirror_alias_read_write();
    test_memory_guest32_write_repairs_stale_host_protection();
    test_memory_guest32_exec_generation_on_write_protect_unmap();
    test_interp_x86_guest32_memory_operands_wrap_to_direct_window();
    test_decode_interp_x86_fs_absolute_store_advances_past_disp32();
    test_interp_x86_call_boundary_exports_eip();
    test_decode_ret_imm16_family();
    test_interp_x86_ret_imm16_cleans_stdcall_stack();
    test_interp_x86_ret_imm16_uses_guest_return_slot();
    test_interp_x64_ret_imm16_cleans_stack();
    test_decode_x86_ff_indirect_branch_family();
    test_interp_x86_ff_indirect_call_jmp_targets();
    test_decode_x86_sse_scalar_move_family();
    test_interp_x86_sse_movsd_load_zeroes_upper_store_low64();
    test_interp_x86_sse_movss_mem_load_zeroes_upper_reg_move_preserves_upper();
    test_decode_lift_x86_notepadpp_scalar_sse_boundary();
    test_decode_lift_x86_notepadpp_x87_comisd_lahf_boundary();
    test_interp_x86_scalar_sse_convert_arith_compare_family();
    test_interp_x86_movlps_low_qword_family_rsaenh();
    test_interp_x86_external_jcc_boundary_exports_eip();
    test_x86_context_x87_defaults();
    test_x87_fldcw_fnstcw_fistp_rounding_modes();
    test_x87_environment_control_state_helpers();
    test_decode_interp_x86_x87_frndint_helper();
    test_decode_x86_x87_environment_control_family();
    test_decode_x64_x87_environment_control_family();
    test_interp_x64_x87_environment_control_family();
    test_interp_x86_x87_load_store_control_conversion_core();
    test_interp_x86_x87_memory_arithmetic_compare_core();
    test_interp_x86_x87_stack_register_pop_core();
    test_interp_x86_x87_environment_control_family();
    test_wow64cpu_process_thread_contract_spine();
    test_wow64cpu_i386_context_roundtrip();
    test_wow64cpu_memory_notify_updates_guest32_vma();
    test_wow64cpu_simulate_runs_i386_guest32_block();
    test_wow64cpu_simulate_chains_control_transfer_blocks();
    test_wow64cpu_simulate_fastpaths_wine_x86_wcslen_pattern();
    test_wow64cpu_simulate_fastpaths_wine_x86_rtl_query_environment_variable_u();
    test_wow64cpu_simulate_fastpaths_wine_x86_strcmp_pattern();
    test_wow64cpu_simulate_honors_env_block_limit();
    test_x86_abi_calling_convention_stack_contracts();
    test_decode_push_pop();
    test_decode_mov_r8w_imm16_rex_operand_override();
    test_decode_group83_or_ecx_imm8();
    test_decode_div_r32_group_f7();
    test_decode_idiv_r32_group_f7();
    test_decode_xadd_lock_r32_calc_atomic();
    test_interp_x64_lock_xadd_r32_memory();
    test_interp_x64_cmpxchg8b_cmpxchg16b_family();
    test_interp_x64_div_r32_calc_startup();
    test_interp_x64_div_by_zero();
    test_interp_x64_div_overflow();
    test_interp_x64_idiv_signed();
    test_interp_x64_jcc_near_taken();
    test_interp_x64_jcc_near_not_taken();
    test_interp_x64_jcc_near_backward();
    test_lift_x64_calc_near_jcc_stops_at_branch();
    test_decode_x64_x86_loop_jrcxz_family();
    test_interp_x64_loop_jrcxz_family();
    test_jit_x64_loop_jrcxz_family();
    test_decode_lea_r9_rsp_disp8_rex_r();
    test_decode_btr_r32_imm8_group_0fba();
    test_interp_x64_btr_r32_imm8_calc_sign_bit();
    test_decode_bts_r64_reg_notepadpp();
    test_interp_x64_bts_r64_reg_notepadpp();
    test_decode_x86_bit_test_reg_family();
    test_decode_x86_bit_test_imm_group_0fba();
    test_interp_x86_bt_and_btc_family();
    test_decode_x86_bit_scan_family();
    test_interp_x86_bit_scan_family();
    test_decode_x86_xmm_logical_family();
    test_interp_x86_xorps_zeroes_xmm0();
    test_decode_x86_sse_mov_packed_family();
    test_interp_x86_xorps_movups_stack_store();
    test_decode_x86_accumulator_imm_family();
    test_interp_x86_and_eax_imm32_accumulator();
    test_decode_cdq_opcode_99();
    test_interp_x64_cdq_positive_and_negative();
    test_decode_movsx_r32_m8_calc();
    test_interp_x64_movsx_r32_m8_negative_byte();
    test_decode_repne_scasw();
    test_interp_x64_repne_scasw_finds_nul();
    test_jit_mov_add();
    test_jit_push_pop();
    test_jit_x64_native_stack_push_pop_family();
    test_jit_x64_native_ret_stack_family();
    test_jit_x64_native_epilogue_restore_ret_block();
    test_jit_commit_verify_failure_not_marked_executable();
    test_jit_load_unmapped_faults();
    test_jit_store_unmapped_faults();
    test_jit_x64_control_state_store_widths();
    test_jit_pop_unmapped_faults();
    test_jit_push_guard_page_faults();
    test_jit_call_push_fault_preserves_pc();
    test_jit_x64_indirect_call_memory_reads_target();
    test_jit_x64_indirect_jmp_register_reads_target();
    test_interp_call_push_fault_preserves_pc_and_rsp();
    test_jit_jcc_helper_fault_stops_before_pc_update();
    test_legitimate_zero_read_is_not_fault();
    test_block_limit_env_0_unlimited();
    test_block_limit_explicit_fault();
    test_lifter_truncation_not_success();
    test_lifter_truncation_probe_preserves_decoder_position();
    test_notepad_plus_long_init_no_silent_abort();
    test_jit_cmp_jcc();
    test_jit_block_cache_loop();
    test_jit_jcc_not_taken();
    test_aot_cache_roundtrip();
    test_aot_cache_sync_failure_unlinks_temp();
    test_abi_stack_setup();
    test_abi_x64_call_setup();
    test_abi_x64_call_stack_args_shadow_space();
    test_abi_x64_call_leaves_positive_stack_headroom();
    test_x64_abi_setup_stack_unmapped_returns_fault();
    test_x64_abi_setup_shadow_space_guard_page_returns_fault();
    test_x64_abi_setup_success_writes_return_shadow_stack_args();
    test_translation_cache_api_stats_and_module_invalidate();
    test_marker_filters_and_jsonl();
    test_iat_rewriter_apply_and_rollback();
    test_iat_rewriter_denied_and_bridge_stub();
    test_imports_bound_import_walk_by_section_size();
    test_imports_resolve_writes_guest_visible_thunk_target();
    test_iat_rewrite_readonly_page_fails_cleanly();
    test_iat_rewrite_writable_page_passes();
    test_abi_thunk_generator_x64_and_registry();
    test_abi_thunk_generator_x86_signature();
    test_abi_thunk_generator_win32_shapes();
    test_thunk_rejects_guest_va();
    test_thunk_rejects_null();
    test_thunk_rejects_data_pointer();
    test_thunk_accepts_registered_host_fn();
    test_page_fault_dispatcher_ownership_dirty_unload();
    test_page_fault_dispatcher_stress_and_outside();
    test_pe_map_image();
    test_pe_map_image_mprotect_failure_returns_error();
    test_pe_load_rejects_e_lfanew_overflow();
    test_pe_load_rejects_section_table_oob();
    test_pe_map_rejects_section_raw_oob();
    test_pe_relocation_directory_oob_returns_parse();
    test_pe_import_name_rva_bounds_checked();
    test_controlled_execution_mvp();
    test_interp_x64_gs_teb_load();
    test_jit_shl_basic_full();
    test_jit_shr_cf_flag_full();
    test_jit_shl_count_zero_flags_preserved();
    test_diff_shl_jit_vs_interp();
    test_diff_shift_fuzzer_small();
    test_diff_shift_fuzzer_final_boss();
    test_phase1_x64_arith_logic_shift_flags_oracle_fuzzer();
    test_decode_rol_rcx_imm8_calc();
    test_decode_shl_al_imm8_notepadpp();
    test_decode_ror_bl_cl_byte_group_d2();
    test_decode_prefetchw_notepadpp_hint_nop();
    test_decode_prefetcht0_hint_nop();
    test_decode_x86_nop_hint_family();
    test_decode_x86_f6_f7_group3_family();
    test_interp_x86_f6_group3_test_and_mul();
    test_interp_x86_fs_segment_load_uses_teb32_base();
    test_interp_x86_mem_imm_store_and_push_mem();
    test_decode_x86_pop_rm32_family();
    test_interp_x86_pop_rm32_fs_seh_restore();
    test_decode_x86_group1_immediate_family();
    test_interp_x86_group1_and_esp_alignment();
    test_interp_x86_group1_operand16_cmp();
    test_decode_x86_alu_modrm_operand16_family();
    test_decode_x86_test_operand16_family();
    test_decode_x86_bswap_family();
    test_decode_x86_byte_shift_rotate_family();
    test_decode_x86_double_shift_family();
    test_interp_x86_alu_modrm_operand16_preserves_upper();
    test_interp_x86_lock_alu_rmw_family();
    test_interp_x86_bswap_family();
    test_interp_x86_byte_shift_rotate_family();
    test_interp_x86_double_shift_family();
    test_decode_x86_xchg_leave_family();
    test_interp_x86_xchg_leave_family();
    test_decode_x86_segment_mov_family();
    test_interp_x86_segment_mov_family();
    test_decode_x86_pushf_popf_family();
    test_interp_x86_pushf_popf_family();
    test_decode_x86_imul_family();
    test_interp_x86_imul_family();
    test_decode_x86_sse_gpr_transfer_family();
    test_interp_x86_sse_gpr_transfer_family();
    test_decode_x86_sse_prefixed_packed_mov_family();
    test_interp_x86_sse_prefixed_packed_mov_family();
    test_decode_x86_sse_unpack_family();
    test_interp_x86_sse_unpack_family();
    test_decode_x86_sse_shuffle_family();
    test_interp_x86_sse_shuffle_family();
    test_decode_x86_sse_word_insert_extract_family();
    test_interp_x86_sse_word_insert_extract_family();
    test_decode_x86_sse_packed_integer_add_sub_family();
    test_interp_x86_sse_packed_integer_add_sub_family();
    test_decode_x86_sse_compare_mask_family();
    test_interp_x86_pcmpeqb_pmovmskb_notepadpp_vector_compare();
    test_decode_x86_lock_atomic_family();
    test_interp_x86_lock_inc_cmpxchg_critical_section();
    test_interp_x86_loader_section_recursive_owner_jcc();
    test_interp_x86_cmpxchg8b_slist_pair();
    test_decode_x86_movzx_movsx_family();
    test_interp_x86_movzx_movsx_memory_family();
    test_interp_x86_legacy_high8_register_family_frame();
    test_decode_x86_accumulator_moffs_family();
    test_decode_x86_mov_operand16_family();
    test_interp_x86_accumulator_moffs_load_store();
    test_interp_x86_mov_operand16_mem_reg_roundtrip();
    test_interp_x64_rol_ror_basic();
    test_interp_x64_rotate_flags_preserve_zf();
    test_decode_test_cx_imm16_calc_security_cookie();
    test_lift_x64_calc_cookie_guard_stops_at_jne();
    test_diff_lazy_flags_jcc_fuzzer();
    test_decode_rep_stosw_notepadpp_fill();
    test_interp_x64_rep_stosw_notepadpp_fill();
    test_decode_rep_movsq_icon_memcpy();
    test_decode_rep_movsb_icon_memcpy();
    test_decode_x86_string_ops_family();
    test_interp_x64_rep_movsq_icon_memcpy_forward();
    test_interp_x64_rep_movsb_direction_flag_backward();
    test_interp_x86_rep_stosd_signal_stack_clear();
    test_interp_x86_rep_movsb_direction_flag_backward();
    test_decode_repe_cmpsb_string_scan();
    test_decode_lodsq_string_load();
    test_interp_x64_repe_cmpsb_stops_on_mismatch();
    test_interp_x64_repne_cmpsb_stops_on_match();
    test_interp_x64_cmpsw_direction_flag_single_step();
    test_interp_x64_lodsd_zero_extends_eax();
    test_interp_x64_rep_lodsb_direction_flag_backward();
    test_interp_x64_neg_mem32_notepadpp_pointer_math();
    test_decode_x64_cmp_operand16_notepadpp_mode_parser();
    test_decode_x64_alu_operand16_family();
    test_interp_x64_cmp_word_mem_reg_notepadpp_je_taken();
    test_interp_x64_cmp_word_mem_reg_notepadpp_je_not_taken();
    test_decode_x64_fe_byte_inc_dec_family();
    test_decode_x86_fe_byte_inc_dec_family();
    test_interp_x86_fe_byte_inc_dec_family();
    test_decode_x64_ff_inc_dec_operand_size_family();
    test_interp_x64_dec_al_notepadpp_char_class();
    test_interp_x64_inc_m8_fe_family();
    test_interp_x64_inc_r8_rexw_notepadpp_scan_loop();
    test_diff_lazy_flags_setcc_cmovcc_fuzzer();
    test_decode_adc_sbb_lahf_sahf();
    test_jit_adc_sbb_carry_chain();
    test_jit_partial_mov_preserves_upper_bits();
    test_jit_x64_native_scalar_mov_family();
    test_jit_x64_native_store_imm_compact_family();
    test_jit_x64_native_extend_family();
    test_jit_x64_native_xmm_move_family();
    test_jit_neg_al_sbb_mask_notepadpp_mode_parser();
    test_jit_lahf_sahf_roundtrip();
    test_jit_setcc_cmovcc_memory_operands();
    test_interp_x64_movaps_xmm_store();
    test_interp_x64_rip_load_r12_indirect_call();
    test_interp_x64_notepad_scalar_sse_sequence();
    test_decode_movd_xmm_r32_calc();
    test_interp_x64_movd_r32_xmm_notepadpp();
    test_interp_x64_movq_r64_xmm_notepadpp_open_pack();
    test_interp_x64_movq_xmm_r64_rexw_roundtrip();
    test_interp_x64_movq_d6_xmm_store_family_notepadpp();
    test_interp_x64_movq_f3_xmm_load_family_smoke_helper();
    test_interp_x64_movabs_r11_imm64_notepadpp_strcmp();
    test_interp_x64_addr32_lea_notepadpp_strcmp();
    test_interp_x64_bsr_r64_notepadpp();
    test_interp_x64_bsf_zero_preserves_cmovne_guard();
    test_jit_x64_bsf_zero_preserves_cmovne_guard();
    test_jit_x64_scalar_load_width_family();
    test_jit_x64_scalar_memory_segment_family();
    test_jit_x64_direct_user_memory_fast_path_family();
    test_jit_x64_hot_byte_scan_loop_native();
    test_jit_x64_native_scalar_add8_partial_flags();
    test_jit_x64_native_scalar_sub64_flags();
    test_jit_x64_native_scalar_test32_zero_flags();
    test_jit_x64_native_scalar_sub_jcc_pair_taken();
    test_jit_x64_native_scalar_test_jcc_pair_taken();
    test_jit_x64_native_copy_scan_body_fallthrough();
    test_jit_x64_native_copy_scan_body_taken();
    test_jit_x64_native_copy_scan_counted_loop_zero_exit();
    test_jit_x64_native_copy_scan_counted_loop_count_exit();
    test_jit_x64_native_copy_scan_counted_loop_promotes_cache_pair();
    test_jit_x64_native_bounded_scan_loop_promotes_cache_pair();
    test_jit_x64_native_store_count_loop_imm_limit();
    test_jit_x64_native_store_count_loop_reg_limit_src_count();
    test_jit_x64_native_zero_store_update_backedge();
    test_jit_x64_native_logic_rmw_memory_family();
    test_jit_x64_native_mem_imm_test_jcc_pair();
    test_jit_x64_native_mem_imm_test_jcc_pair_word_fallthrough();
    test_jit_x64_native_mem_imm_cmp_jcc_pair();
    test_jit_x64_native_zero_test_jcc_block_family();
    test_jit_x64_native_adjacent_mem64_pair_family();
    test_jit_x64_native_stack_spill_push_sub_prologue();
    test_jit_x64_native_prologue_local_init_test_block();
    test_jit_x64_native_mov_lea_same_base_pair();
    test_jit_x64_native_store_imm_mov_lea_same_base();
    test_jit_x64_native_xmm_load_store_pair();
    test_jit_x64_native_scalar_load_store_pair();
    test_jit_x64_native_test_same_reg_jcc_pair();
    test_jit_x64_hot_word_scan_loop_native();
    test_jit_x64_cmp_mem_operand_routes_to_helper();
    test_jit_x64_mul_div_family_routes_to_helper();
    test_jit_x64_interp_helper_covers_unity_mono_fallback_families();
    test_interp_x64_bswap_family_notepadpp_message_path();
    test_interp_x64_legacy_high8_register_family_strlen_tail();
    test_interp_x64_byte_offset_propagates_through_flag_helpers();
    test_interp_x64_sse_visible_window_family_gaps();
    test_interp_x64_movd_cvtdq2pd_calc_cluster();
    test_interp_x64_calc_sse2_divsd_mulsd_cluster();
    test_interp_x64_addsd_subsd_scalar_double();
    test_interp_x64_movdqa_xmm_store_calc();
    test_interp_x64_movdqu_unaligned_xmm_move_blit_path();
    test_interp_x64_scalar_sse_move_store_width_notepadpp_paint();
    test_interp_x64_scalar_sse_mem_load_zeroes_upper_reg_move_preserves_upper();
    test_interp_x64_movlps_low_qword_family_rsaenh();
    test_interp_x64_movhps_movlhps_qword_lane_family_unityplayer();
    test_interp_x64_psub_integer_family_smoke_helper_toolbar();
    test_interp_x64_pcmpgt_integer_family_smoke_helper_toolbar();
    test_interp_x64_movmsk_fp_sign_extract_family_smoke_helper_toolbar();
    test_interp_x64_addpd_subpd_notepadpp_callback_block();
    test_interp_x64_addps_subps_addss_subss_family();
    test_interp_x64_mov_imm32_rip_relative_store_calc_crt_state();
    test_decode_x64_rip_relative_trailing_immediate_family();
    test_interp_x64_xorps_zero_startup_block();
    test_interp_x64_pxor_zero_calc_vector_path();
    test_interp_x64_xmm_bitwise_logical_family_notepadpp();
    test_interp_x64_unpck_fp_family_callback_path();
    test_interp_x64_punpcklqdq_notepadpp_broadcast();
    test_interp_x64_punpck_integer_family_notepadpp_vector_unpack();
    test_interp_x64_pack_integer_saturation_family_blit_path();
    test_interp_x64_pmul_integer_family_blit_path();
    test_interp_x64_saturating_add_average_shuffle_blit_batch();
    test_interp_x64_pshuf_immediate_family_notepadpp_vector_path();
    test_interp_x64_psrldq_notepadpp_shift_bytes();
    test_interp_x64_packed_immediate_shift_family_smoke_helper_toolbar();
    test_interp_x64_pcmpeqw_pmovmskb_notepadpp_vector_compare();
    test_interp_x64_cvtsi2ss_mulss_comiss_calc_cluster();
    test_interp_x64_divss_notepadpp_scalar_float();
    test_decode_x64_sqrt_family_0f51();
    test_interp_x64_sqrtss_unityplayer_scalar_float();
    test_decode_x64_rsqrt_rcp_family_0f52_0f53();
    test_interp_x64_rsqrtps_rcpss_reciprocal_estimate_family();
    test_decode_x64_mul_div_packed_fp_family_0f59_0f5e();
    test_interp_x64_divps_mulpd_packed_fp_family();
    test_decode_x64_shufps_shufpd_family_0fc6();
    test_interp_x64_shufps_shufpd_family();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
