/* HyperBridge C test runner */
#include "hb_context.h"
#include "hb_decoder.h"
#include "hb_lifter.h"
#include "hb_ir.h"
#include "hb_runtime.h"
#include "hb_memory.h"
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

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); tests_failed++; return; } } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) { fprintf(stderr, "FAIL: %s:%d: expected %llu, got %llu\n", __FILE__, __LINE__, (unsigned long long)(b), (unsigned long long)(a)); tests_failed++; return; } } while(0)

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
    ASSERT(out.result == HB_ERR_NOT_FOUND);
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
    ASSERT(out.result == HB_ERR_NOT_FOUND);
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
    ASSERT(out.result == HB_ERR_NOT_FOUND);
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
    ASSERT(out.result == HB_ERR_NOT_FOUND);
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
    ASSERT(out.result == HB_ERR_NOT_FOUND);
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

/* --- Entry point --- */

int main(void) {
    printf("HyperBridge C test runner\n");
    test_decode_mov_reg_reg();
    test_decode_mov_r8_mem16_operand_override();
    test_decode_cpuid_opcode_0fa2();
    test_decode_x64_truncated_sib_fails_cleanly();
    test_decode_xgetbv_opcode_0f01d0();
    test_interp_x64_cpuid_vendor_and_leaf1();
    test_jit_x64_cpuid_vendor_and_leaf1();
    test_interp_x64_xgetbv_leaf0();
    test_jit_x64_xgetbv_leaf0();
    test_decode_add_imm();
    test_decode_x64_accumulator_imm_logic_family();
    test_interp_x64_and_eax_imm32_calc_callback();
    test_interp_x64_linear_block_advances_pc();
    test_memory_special_write_unmapped_live_range();
    test_memory_cross_region_write_read_span();
    test_decode_push_pop();
    test_decode_mov_r8w_imm16_rex_operand_override();
    test_decode_group83_or_ecx_imm8();
    test_decode_div_r32_group_f7();
    test_decode_idiv_r32_group_f7();
    test_decode_xadd_lock_r32_calc_atomic();
    test_interp_x64_lock_xadd_r32_memory();
    test_interp_x64_div_r32_calc_startup();
    test_interp_x64_div_by_zero();
    test_interp_x64_div_overflow();
    test_interp_x64_idiv_signed();
    test_interp_x64_jcc_near_taken();
    test_interp_x64_jcc_near_not_taken();
    test_interp_x64_jcc_near_backward();
    test_lift_x64_calc_near_jcc_stops_at_branch();
    test_decode_lea_r9_rsp_disp8_rex_r();
    test_decode_btr_r32_imm8_group_0fba();
    test_interp_x64_btr_r32_imm8_calc_sign_bit();
    test_decode_bts_r64_reg_notepadpp();
    test_interp_x64_bts_r64_reg_notepadpp();
    test_decode_cdq_opcode_99();
    test_interp_x64_cdq_positive_and_negative();
    test_decode_movsx_r32_m8_calc();
    test_interp_x64_movsx_r32_m8_negative_byte();
    test_decode_repne_scasw();
    test_interp_x64_repne_scasw_finds_nul();
    test_jit_mov_add();
    test_jit_push_pop();
    test_jit_commit_verify_failure_not_marked_executable();
    test_jit_load_unmapped_faults();
    test_jit_store_unmapped_faults();
    test_jit_pop_unmapped_faults();
    test_jit_push_guard_page_faults();
    test_jit_call_push_fault_preserves_pc();
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
    test_decode_rol_rcx_imm8_calc();
    test_decode_shl_al_imm8_notepadpp();
    test_decode_ror_bl_cl_byte_group_d2();
    test_decode_prefetchw_notepadpp_hint_nop();
    test_decode_prefetcht0_hint_nop();
    test_interp_x64_rol_ror_basic();
    test_interp_x64_rotate_flags_preserve_zf();
    test_decode_test_cx_imm16_calc_security_cookie();
    test_lift_x64_calc_cookie_guard_stops_at_jne();
    test_diff_lazy_flags_jcc_fuzzer();
    test_decode_rep_stosw_notepadpp_fill();
    test_interp_x64_rep_stosw_notepadpp_fill();
    test_interp_x64_neg_mem32_notepadpp_pointer_math();
    test_decode_x64_cmp_operand16_notepadpp_mode_parser();
    test_decode_x64_alu_operand16_family();
    test_interp_x64_cmp_word_mem_reg_notepadpp_je_taken();
    test_interp_x64_cmp_word_mem_reg_notepadpp_je_not_taken();
    test_decode_x64_fe_byte_inc_dec_family();
    test_decode_x64_ff_inc_dec_operand_size_family();
    test_interp_x64_dec_al_notepadpp_char_class();
    test_interp_x64_inc_m8_fe_family();
    test_interp_x64_inc_r8_rexw_notepadpp_scan_loop();
    test_diff_lazy_flags_setcc_cmovcc_fuzzer();
    test_decode_adc_sbb_lahf_sahf();
    test_jit_adc_sbb_carry_chain();
    test_jit_partial_mov_preserves_upper_bits();
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
    test_interp_x64_bswap_family_notepadpp_message_path();
    test_interp_x64_legacy_high8_register_family_strlen_tail();
    test_interp_x64_byte_offset_propagates_through_flag_helpers();
    test_interp_x64_sse_visible_window_family_gaps();
    test_interp_x64_movd_cvtdq2pd_calc_cluster();
    test_interp_x64_calc_sse2_divsd_mulsd_cluster();
    test_interp_x64_addsd_subsd_scalar_double();
    test_interp_x64_movdqa_xmm_store_calc();
    test_interp_x64_scalar_sse_move_store_width_notepadpp_paint();
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
    test_interp_x64_pshuf_immediate_family_notepadpp_vector_path();
    test_interp_x64_psrldq_notepadpp_shift_bytes();
    test_interp_x64_packed_immediate_shift_family_smoke_helper_toolbar();
    test_interp_x64_pcmpeqw_pmovmskb_notepadpp_vector_compare();
    test_interp_x64_cvtsi2ss_mulss_comiss_calc_cluster();
    test_interp_x64_divss_notepadpp_scalar_float();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
