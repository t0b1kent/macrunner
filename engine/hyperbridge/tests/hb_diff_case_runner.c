#include "hb_context.h"
#include "hb_decoder.h"
#include "hb_flags.h"
#include "hb_ir.h"
#include "hb_lifter.h"
#include "hb_memory.h"
#include "hb_runtime.h"
#include "hb_x87.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB_DIFF_CODE_BASE  0x100000ULL
#define HB_DIFF_DATA_BASE  0x70000000ULL
#define HB_DIFF_STACK_BASE 0x71000000ULL
#define HB_DIFF_DATA_SIZE  0x2000U
#define HB_DIFF_STACK_SIZE 0x2000U
#define HB_DIFF_MAX_CODE   15U
#define HB_DIFF_FNV64_OFFSET 0xcbf29ce484222325ULL
#define HB_DIFF_FNV64_PRIME  0x100000001b3ULL
#define HB_DIFF_RFLAGS_FUZZ_MASK 0xcd5ULL

typedef struct {
    hb_arch_t arch;
    uint64_t gpr[16];
    uint64_t rip;
    uint32_t eip;
    hb_flags_t flags;
    uint64_t rflags;
    uint32_t flag_mask;
    hb_result_t flag_status;
    bool lazy_pending;
    uint8_t lazy_kind;
    uint8_t lazy_width;
    uint64_t lazy_lhs;
    uint64_t lazy_rhs;
    uint64_t lazy_result;
    uint64_t lazy_count;
    uint8_t xmm[16][16];
    uint8_t ymm_hi[16][16];
    uint8_t zmm_hi[16][32];
    uint8_t xmm_ext[16][16];
    uint8_t ymm_hi_ext[16][16];
    uint8_t zmm_hi_ext[16][32];
    uint64_t k[8];
    uint16_t x87_cw;
    uint16_t x87_sw;
    uint16_t x87_tag;
    uint8_t data[HB_DIFF_DATA_SIZE];
    uint8_t stack[HB_DIFF_STACK_SIZE];
    hb_result_t api_result;
    hb_result_t exec_result;
} hb_diff_snapshot_t;

static uint64_t splitmix64_next(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_hex_bytes(const char* s, uint8_t* out, size_t max_out, size_t* out_len) {
    size_t n = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        int hi = hex_nibble(*s++);
        if (hi < 0 || !*s) return false;
        int lo = hex_nibble(*s++);
        if (lo < 0 || n >= max_out) return false;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n;
    return n > 0;
}

static bool parse_hex(const char* s, uint8_t* out, size_t* out_len) {
    return parse_hex_bytes(s, out, HB_DIFF_MAX_CODE, out_len);
}

static void print_hex_bytes(const uint8_t* bytes, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        putchar(hex[bytes[i] >> 4]);
        putchar(hex[bytes[i] & 0xf]);
    }
}

static uint64_t fnv1a64(const uint8_t* bytes, size_t len) {
    uint64_t h = HB_DIFF_FNV64_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= HB_DIFF_FNV64_PRIME;
    }
    return h;
}

static void set_flags_from_bits(hb_context_t* ctx, uint64_t bits) {
    ctx->flags.cf = (bits & 0x001) != 0;
    ctx->flags.pf = (bits & 0x004) != 0;
    ctx->flags.af = (bits & 0x010) != 0;
    ctx->flags.zf = (bits & 0x040) != 0;
    ctx->flags.sf = (bits & 0x080) != 0;
    ctx->flags.of = (bits & 0x800) != 0;
    uint64_t image = (bits & HB_DIFF_RFLAGS_FUZZ_MASK) | 0x202ULL;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eflags = (uint32_t)image;
    else ctx->regs.x64.rflags = image;
}

static uint64_t flags_to_bits(const hb_flags_t* f) {
    return (f->cf ? 0x001ULL : 0) |
           (f->pf ? 0x004ULL : 0) |
           (f->af ? 0x010ULL : 0) |
           (f->zf ? 0x040ULL : 0) |
           (f->sf ? 0x080ULL : 0) |
           (f->of ? 0x800ULL : 0);
}

static uint64_t flag_mask_to_bits(uint32_t mask) {
    uint64_t bits = 0;
    if (mask & HB_FLAG_BIT_CF) bits |= 0x001ULL;
    if (mask & HB_FLAG_BIT_PF) bits |= 0x004ULL;
    if (mask & HB_FLAG_BIT_AF) bits |= 0x010ULL;
    if (mask & HB_FLAG_BIT_ZF) bits |= 0x040ULL;
    if (mask & HB_FLAG_BIT_SF) bits |= 0x080ULL;
    if (mask & HB_FLAG_BIT_OF) bits |= 0x800ULL;
    return bits;
}

static void fill_random(uint64_t* rng, uint8_t* out, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint64_t v = splitmix64_next(rng);
        size_t n = len - off < sizeof(v) ? len - off : sizeof(v);
        memcpy(out + off, &v, n);
        off += n;
    }
}

static hb_result_t init_context(hb_context_t* ctx, uint64_t seed, const uint8_t* code, size_t code_len) {
    uint64_t rng = seed;
    uint8_t data[HB_DIFF_DATA_SIZE];
    uint8_t stack[HB_DIFF_STACK_SIZE];
    ctx->memory = hb_memory_create(0x200000);
    if (!ctx->memory) return HB_ERR_OUT_OF_MEMORY;
    hb_result_t r = hb_memory_map_private(ctx->memory, HB_DIFF_CODE_BASE, 0x1000,
                                          HB_PERM_READ | HB_PERM_WRITE);
    if (r != HB_OK) return r;
    r = hb_memory_write(ctx->memory, HB_DIFF_CODE_BASE, code, code_len);
    if (r != HB_OK) return r;
    r = hb_memory_protect(ctx->memory, HB_DIFF_CODE_BASE, 0x1000, HB_PERM_READ | HB_PERM_EXEC);
    if (r != HB_OK) return r;
    r = hb_memory_map_private(ctx->memory, HB_DIFF_DATA_BASE, HB_DIFF_DATA_SIZE,
                              HB_PERM_READ | HB_PERM_WRITE);
    if (r != HB_OK) return r;
    r = hb_memory_map_private(ctx->memory, HB_DIFF_STACK_BASE, HB_DIFF_STACK_SIZE,
                              HB_PERM_READ | HB_PERM_WRITE);
    if (r != HB_OK) return r;
    fill_random(&rng, data, sizeof(data));
    fill_random(&rng, stack, sizeof(stack));
    /* Optional override: HB_DIFF_DATA_HEX=<seed>:<hex> lets a fuzz harness
     * pre-populate the data region deterministically (so both engines
     * see the same bytes for a given seed). The seed is encoded so the
     * fuzzer can verify the right env var was used. */
    const char* data_hex_env = getenv("HB_DIFF_DATA_HEX");
    if (data_hex_env) {
        const char* colon = strchr(data_hex_env, ':');
        if (colon) {
            uint64_t env_seed = strtoull(data_hex_env, NULL, 0);
            if (env_seed == seed) {
                const char* hex = colon + 1;
                size_t hex_len = strlen(hex);
                size_t want = sizeof(data) * 2;
                if (hex_len == want) {
                    size_t parsed = 0;
                    if (parse_hex_bytes(hex, data, sizeof(data), &parsed) && parsed == sizeof(data)) {
                        /* data replaced with fuzzer-provided bytes */
                    }
                }
            }
        }
    }
    r = hb_memory_write(ctx->memory, HB_DIFF_DATA_BASE, data, sizeof(data));
    if (r != HB_OK) return r;
    r = hb_memory_write(ctx->memory, HB_DIFF_STACK_BASE, stack, sizeof(stack));
    if (r != HB_OK) return r;

    if (ctx->mode == HB_MODE_32BIT) {
        ctx->regs.x86.eax = (uint32_t)(HB_DIFF_DATA_BASE + 0x1000);
        ctx->regs.x86.ebx = (uint32_t)splitmix64_next(&rng);
        ctx->regs.x86.ecx = (uint32_t)((splitmix64_next(&rng) & 0x0fU) + 1U);
        ctx->regs.x86.edx = 0;
        ctx->regs.x86.esi = (uint32_t)(HB_DIFF_DATA_BASE + 0x0800);
        ctx->regs.x86.edi = (uint32_t)(HB_DIFF_DATA_BASE + 0x1000);
        ctx->regs.x86.esp = (uint32_t)(HB_DIFF_STACK_BASE + 0x1000);
        ctx->regs.x86.ebp = (uint32_t)(HB_DIFF_STACK_BASE + 0x1100);
        ctx->regs.x86.eip = (uint32_t)HB_DIFF_CODE_BASE;
        /* Reset x87 state to FNINIT semantics so the tag word starts as
         * 0xFFFF (all empty), control word 0x037F, status word 0, TOP=0.
         * Without this the C-initialized fields are 0 → tag_word=0 → all
         * "valid", which diverges from the per-architecture FNINIT spec
         * the x87 interpreter (and Unicorn) expects. */
        hb_x87_fninit(&ctx->regs.x86.x87);
    } else {
        ctx->regs.x64.rax = HB_DIFF_DATA_BASE + 0x1000;
        ctx->regs.x64.rbx = splitmix64_next(&rng);
        ctx->regs.x64.rcx = (splitmix64_next(&rng) & 0x3fU) + 1U;
        ctx->regs.x64.rdx = splitmix64_next(&rng);
        ctx->regs.x64.rsi = HB_DIFF_DATA_BASE + 0x0400;
        ctx->regs.x64.rdi = HB_DIFF_DATA_BASE + 0x1000;
        ctx->regs.x64.rsp = HB_DIFF_STACK_BASE + 0x1000;
        ctx->regs.x64.rbp = HB_DIFF_STACK_BASE + 0x1100;
        ctx->regs.x64.r8 = splitmix64_next(&rng);
        ctx->regs.x64.r9 = splitmix64_next(&rng);
        ctx->regs.x64.r10 = splitmix64_next(&rng);
        ctx->regs.x64.r11 = splitmix64_next(&rng);
        ctx->regs.x64.r12 = splitmix64_next(&rng);
        ctx->regs.x64.r13 = splitmix64_next(&rng);
        ctx->regs.x64.r14 = splitmix64_next(&rng);
        ctx->regs.x64.r15 = splitmix64_next(&rng);
        ctx->regs.x64.rip = HB_DIFF_CODE_BASE;
    }
    ctx->pc = HB_DIFF_CODE_BASE;
    set_flags_from_bits(ctx, splitmix64_next(&rng));
    for (unsigned i = 0; i < 16; i++) {
        if (ctx->mode == HB_MODE_32BIT) {
            uint8_t tmp[16];
            fill_random(&rng, tmp, sizeof(tmp));
            if (i < 8) memcpy(ctx->regs.x86.xmm[i], tmp, sizeof(tmp));
        } else {
            fill_random(&rng, (uint8_t*)ctx->regs.x64.xmm[i], 16);
            fill_random(&rng, (uint8_t*)ctx->xmm_ext[i], 16);
            fill_random(&rng, (uint8_t*)ctx->ymm_hi_ext[i], 16);
            fill_random(&rng, (uint8_t*)ctx->zmm_hi_ext[i], 32);
        }
        fill_random(&rng, (uint8_t*)ctx->ymm_hi[i], 16);
        fill_random(&rng, (uint8_t*)ctx->zmm_hi[i], 32);
    }
    for (unsigned i = 0; i < 8; i++) ctx->k[i] = splitmix64_next(&rng);
    return HB_OK;
}

static hb_result_t lift_code(hb_arch_t arch, const uint8_t* code, size_t code_len, hb_ir_func_t** out_func) {
    hb_decoder_t* dec = hb_decoder_create(arch, code, code_len, HB_DIFF_CODE_BASE);
    if (!dec) return HB_ERR_OUT_OF_MEMORY;
    hb_result_t r = arch == HB_ARCH_X86 ? hb_lift_func_x86(dec, out_func) : hb_lift_func_x64(dec, out_func);
    hb_decoder_destroy(dec);
    return r;
}

static void capture_snapshot(hb_context_t* ctx, hb_diff_snapshot_t* s,
                              hb_result_t api_result, hb_exec_result_t* exec) {
    memset(s, 0, sizeof(*s));
    s->arch = ctx->arch;
    s->lazy_pending = ctx->lazy_flags.pending;
    s->lazy_kind = (uint8_t)ctx->lazy_flags.kind;
    s->lazy_width = ctx->lazy_flags.width;
    s->lazy_lhs = ctx->lazy_flags.lhs;
    s->lazy_rhs = ctx->lazy_flags.rhs;
    s->lazy_result = ctx->lazy_flags.result;
    s->lazy_count = ctx->lazy_flags.count;
    s->flag_mask = ctx->lazy_flags.pending ? ctx->lazy_flags.valid_mask : HB_FLAG_BIT_ALL;
    s->flag_status = hb_lazy_flags_materialize(ctx, s->flag_mask);
    if (ctx->mode == HB_MODE_32BIT) {
        s->gpr[0] = ctx->regs.x86.eax; s->gpr[1] = ctx->regs.x86.ebx;
        s->gpr[2] = ctx->regs.x86.ecx; s->gpr[3] = ctx->regs.x86.edx;
        s->gpr[4] = ctx->regs.x86.esi; s->gpr[5] = ctx->regs.x86.edi;
        s->gpr[6] = ctx->regs.x86.esp; s->gpr[7] = ctx->regs.x86.ebp;
        s->eip = (uint32_t)ctx->pc;
        s->rflags = ctx->regs.x86.eflags;
        for (unsigned i = 0; i < 8; i++) memcpy(s->xmm[i], ctx->regs.x86.xmm[i], 16);
        s->x87_cw = ctx->regs.x86.x87.control_word;
        s->x87_sw = ctx->regs.x86.x87.status_word;
        s->x87_tag = ctx->regs.x86.x87.tag_word;
    } else {
        s->gpr[0] = ctx->regs.x64.rax; s->gpr[1] = ctx->regs.x64.rbx;
        s->gpr[2] = ctx->regs.x64.rcx; s->gpr[3] = ctx->regs.x64.rdx;
        s->gpr[4] = ctx->regs.x64.rsi; s->gpr[5] = ctx->regs.x64.rdi;
        s->gpr[6] = ctx->regs.x64.rsp; s->gpr[7] = ctx->regs.x64.rbp;
        s->gpr[8] = ctx->regs.x64.r8;  s->gpr[9] = ctx->regs.x64.r9;
        s->gpr[10] = ctx->regs.x64.r10; s->gpr[11] = ctx->regs.x64.r11;
        s->gpr[12] = ctx->regs.x64.r12; s->gpr[13] = ctx->regs.x64.r13;
        s->gpr[14] = ctx->regs.x64.r14; s->gpr[15] = ctx->regs.x64.r15;
        s->rflags = ctx->regs.x64.rflags;
        for (unsigned i = 0; i < 16; i++) memcpy(s->xmm[i], ctx->regs.x64.xmm[i], 16);
        for (unsigned i = 0; i < 16; i++) memcpy(s->xmm_ext[i], ctx->xmm_ext[i], 16);
        for (unsigned i = 0; i < 16; i++) memcpy(s->ymm_hi_ext[i], ctx->ymm_hi_ext[i], 16);
        for (unsigned i = 0; i < 16; i++) memcpy(s->zmm_hi_ext[i], ctx->zmm_hi_ext[i], 32);
    }
    s->rip = ctx->pc;
    s->flags = ctx->flags;
    for (unsigned i = 0; i < 16; i++) memcpy(s->ymm_hi[i], ctx->ymm_hi[i], 16);
    for (unsigned i = 0; i < 16; i++) memcpy(s->zmm_hi[i], ctx->zmm_hi[i], 32);
    for (unsigned i = 0; i < 8; i++) s->k[i] = ctx->k[i];
    if (ctx->memory) {
        (void)hb_memory_read(ctx->memory, HB_DIFF_DATA_BASE, s->data, sizeof(s->data));
        (void)hb_memory_read(ctx->memory, HB_DIFF_STACK_BASE, s->stack, sizeof(s->stack));
    }
    s->api_result = api_result;
    s->exec_result = exec ? exec->result : api_result;
}

static hb_result_t run_backend(hb_arch_t arch, const uint8_t* code, size_t code_len, uint64_t seed,
                               hb_backend_t backend, hb_diff_snapshot_t* initial,
                               hb_diff_snapshot_t* final) {
    hb_ir_func_t* func = NULL;
    hb_result_t r = lift_code(arch, code, code_len, &func);
    if (r != HB_OK) return r;
    const char* dump_ir = getenv("HB_DIFF_DUMP_IR");
    if (dump_ir && strcmp(dump_ir, "1") == 0) {
        char* s = hb_ir_func_to_string(func);
        if (s) {
            fputs(s, stderr);
            free(s);
        }
        if (func->cfg && func->cfg->entry) {
            hb_ir_block_t* blk = func->cfg->entry;
            for (size_t i = 0; i < blk->instr_count; i++) {
                const hb_ir_instr_t* ins = &blk->instrs[i];
                fprintf(stderr,
                        "  instr %zu op=%u dst(type=%u reg=%u size=%u) src1(type=%u reg=%u size=%u) src2(type=%u reg=%u imm=%lld size=%u)\n",
                        i, (unsigned)ins->op,
                        (unsigned)ins->dst.type, (unsigned)ins->dst.reg, (unsigned)ins->dst.size,
                        (unsigned)ins->src1.type, (unsigned)ins->src1.reg, (unsigned)ins->src1.size,
                        (unsigned)ins->src2.type, (unsigned)ins->src2.reg,
                        (long long)ins->src2.imm, (unsigned)ins->src2.size);
            }
        }
    }
    hb_context_t* ctx = hb_context_create(arch, backend);
    if (!ctx) {
        hb_ir_func_destroy(func);
        return HB_ERR_OUT_OF_MEMORY;
    }
    r = init_context(ctx, seed, code, code_len);
    if (r == HB_OK && initial) capture_snapshot(ctx, initial, HB_OK, NULL);
    hb_exec_result_t exec;
    memset(&exec, 0, sizeof(exec));
    hb_result_t api = r == HB_OK ? hb_runtime_run(ctx, func, backend, &exec) : r;
    capture_snapshot(ctx, final, api, &exec);
    hb_context_destroy(ctx);
    hb_ir_func_destroy(func);
    return api;
}

static const char* reg_name(hb_arch_t arch, unsigned i) {
    static const char* names64[16] = {
        "rax","rbx","rcx","rdx","rsi","rdi","rsp","rbp",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    static const char* names32[8] = {
        "eax","ebx","ecx","edx","esi","edi","esp","ebp"
    };
    return arch == HB_ARCH_X86 ? names32[i] : names64[i];
}

static unsigned reg_count(hb_arch_t arch) {
    return arch == HB_ARCH_X86 ? 8u : 16u;
}

static bool snapshots_equal(const hb_diff_snapshot_t* a, const hb_diff_snapshot_t* b,
                            char* field, size_t field_size) {
    if (a->api_result != b->api_result) {
        snprintf(field, field_size, "api_result:%d:%d", a->api_result, b->api_result);
        return false;
    }
    if (a->exec_result != b->exec_result) {
        snprintf(field, field_size, "exec_result:%d:%d", a->exec_result, b->exec_result);
        return false;
    }
    if (a->flag_status != b->flag_status) {
        snprintf(field, field_size, "flag_status:%d:%d", a->flag_status, b->flag_status);
        return false;
    }
    for (unsigned i = 0; i < reg_count(a->arch); i++) {
        if (a->gpr[i] != b->gpr[i]) {
            snprintf(field, field_size, "reg:%s", reg_name(a->arch, i));
            return false;
        }
    }
    if (a->flag_mask != b->flag_mask) {
        snprintf(field, field_size, "flag_mask:0x%x:0x%x", a->flag_mask, b->flag_mask);
        return false;
    }
    uint64_t flag_bits_mask = flag_mask_to_bits(a->flag_mask);
    if (((flags_to_bits(&a->flags) ^ flags_to_bits(&b->flags)) & flag_bits_mask) != 0) {
        snprintf(field, field_size, "flags");
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (memcmp(a->xmm[i], b->xmm[i], 16) != 0) {
            snprintf(field, field_size, "xmm%u", i);
            return false;
        }
        if (memcmp(a->ymm_hi[i], b->ymm_hi[i], 16) != 0) {
            snprintf(field, field_size, "ymm_hi%u", i);
            return false;
        }
        if (memcmp(a->zmm_hi[i], b->zmm_hi[i], 32) != 0) {
            snprintf(field, field_size, "zmm_hi%u", i);
            return false;
        }
        if (memcmp(a->xmm_ext[i], b->xmm_ext[i], 16) != 0) {
            snprintf(field, field_size, "xmm%u", i + 16);
            return false;
        }
        if (memcmp(a->ymm_hi_ext[i], b->ymm_hi_ext[i], 16) != 0) {
            snprintf(field, field_size, "ymm_hi%u", i + 16);
            return false;
        }
        if (memcmp(a->zmm_hi_ext[i], b->zmm_hi_ext[i], 32) != 0) {
            snprintf(field, field_size, "zmm_hi%u", i + 16);
            return false;
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (a->k[i] != b->k[i]) {
            snprintf(field, field_size, "k%u", i);
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(a->data); i++) {
        if (a->data[i] != b->data[i]) {
            snprintf(field, field_size, "data+0x%zx", i);
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(a->stack); i++) {
        if (a->stack[i] != b->stack[i]) {
            snprintf(field, field_size, "stack+0x%zx", i);
            return false;
        }
    }
    return true;
}

static void print_regs_json(const hb_diff_snapshot_t* s) {
    putchar('{');
    for (unsigned i = 0; i < reg_count(s->arch); i++) {
        if (i) putchar(',');
        if (s->arch == HB_ARCH_X86) printf("\"%s\":\"0x%08" PRIx64 "\"", reg_name(s->arch, i), s->gpr[i] & 0xffffffffULL);
        else printf("\"%s\":\"0x%016" PRIx64 "\"", reg_name(s->arch, i), s->gpr[i]);
    }
    if (s->arch == HB_ARCH_X86) printf(",\"eip\":\"0x%08" PRIx32 "\"}", s->eip);
    else printf(",\"rip\":\"0x%016" PRIx64 "\"}", s->rip);
}

static void print_vec_array_json(const uint8_t v[16][16]) {
    putchar('[');
    for (unsigned i = 0; i < 16; i++) {
        if (i) putchar(',');
        putchar('"');
        print_hex_bytes(v[i], 16);
        putchar('"');
    }
    putchar(']');
}

static void print_zmm_hi_array_json(const uint8_t v[16][32]) {
    putchar('[');
    for (unsigned i = 0; i < 16; i++) {
        if (i) putchar(',');
        putchar('"');
        print_hex_bytes(v[i], 32);
        putchar('"');
    }
    putchar(']');
}

static void print_k_array_json(const uint64_t k[8]) {
    putchar('[');
    for (unsigned i = 0; i < 8; i++) {
        if (i) putchar(',');
        printf("\"0x%016" PRIx64 "\"", k[i]);
    }
    putchar(']');
}

static void print_flags_json(const hb_flags_t* f) {
    printf("{\"cf\":%u,\"pf\":%u,\"af\":%u,\"zf\":%u,\"sf\":%u,\"of\":%u}",
           f->cf ? 1 : 0, f->pf ? 1 : 0, f->af ? 1 : 0,
           f->zf ? 1 : 0, f->sf ? 1 : 0, f->of ? 1 : 0);
}

static void print_snapshot_json(const char* name, const hb_diff_snapshot_t* s) {
    printf("\"%s\":{\"api\":%d,\"result\":%d,\"regs\":", name, s->api_result, s->exec_result);
    print_regs_json(s);
    printf(",\"rflags\":\"0x%016" PRIx64 "\"", s->rflags);
    printf(",\"flags\":");
    print_flags_json(&s->flags);
    printf(",\"flag_mask\":\"0x%02x\",\"flag_status\":%d", s->flag_mask, s->flag_status);
    printf(",\"lazy\":{\"pending\":%u,\"kind\":%u,\"width\":%u,"
           "\"lhs\":\"0x%016" PRIx64 "\",\"rhs\":\"0x%016" PRIx64 "\","
           "\"result\":\"0x%016" PRIx64 "\",\"count\":\"0x%016" PRIx64 "\"}",
           s->lazy_pending ? 1u : 0u, s->lazy_kind, s->lazy_width,
           s->lazy_lhs, s->lazy_rhs, s->lazy_result, s->lazy_count);
    printf(",\"xmm\":");
    print_vec_array_json(s->xmm);
    printf(",\"ymm_hi\":");
    print_vec_array_json(s->ymm_hi);
    printf(",\"zmm_hi\":");
    print_zmm_hi_array_json(s->zmm_hi);
    printf(",\"xmm_ext\":");
    print_vec_array_json(s->xmm_ext);
    printf(",\"ymm_hi_ext\":");
    print_vec_array_json(s->ymm_hi_ext);
    printf(",\"zmm_hi_ext\":");
    print_zmm_hi_array_json(s->zmm_hi_ext);
    printf(",\"k\":");
    print_k_array_json(s->k);
    printf(",\"data_hash\":\"0x%016" PRIx64 "\",\"stack_hash\":\"0x%016" PRIx64 "\"",
           fnv1a64(s->data, sizeof(s->data)), fnv1a64(s->stack, sizeof(s->stack)));
    printf(",\"xmm0\":\"");
    print_hex_bytes(s->xmm[0], 16);
    printf("\",\"ymm0_hi\":\"");
    print_hex_bytes(s->ymm_hi[0], 16);
    printf("\",\"zmm0_hi\":\"");
    print_hex_bytes(s->zmm_hi[0], 32);
    printf("\"");
    if (s->arch == HB_ARCH_X86) {
        printf(",\"x87\":{\"cw\":\"0x%04x\",\"sw\":\"0x%04x\",\"tag\":\"0x%04x\"}",
               s->x87_cw, s->x87_sw, s->x87_tag);
    }
    const char* dump_memory = getenv("HB_DIFF_DUMP_MEMORY");
    if (dump_memory && strcmp(dump_memory, "1") == 0) {
        printf(",\"data\":\"");
        print_hex_bytes(s->data, sizeof(s->data));
        printf("\",\"stack\":\"");
        print_hex_bytes(s->stack, sizeof(s->stack));
        printf("\"");
    }
    printf("}");
}

static void run_case_line(hb_arch_t arch, uint64_t seed, const char* code_hex) {
    uint8_t code[HB_DIFF_MAX_CODE];
    size_t code_len = 0;
    if (!parse_hex(code_hex, code, &code_len)) {
        printf("{\"ok\":false,\"error\":\"bad_hex\"}\n");
        return;
    }
    hb_diff_snapshot_t initial, interp, jit;
    memset(&initial, 0, sizeof(initial));
    hb_result_t ri = run_backend(arch, code, code_len, seed, HB_BACKEND_INTERP, &initial, &interp);
    const char* interp_only_env = getenv("HB_DIFF_INTERP_ONLY");
    bool interp_only = interp_only_env && strcmp(interp_only_env, "1") == 0;
    hb_result_t rj = HB_OK;
    if (interp_only) {
        jit = interp;
    } else {
        rj = run_backend(arch, code, code_len, seed, HB_BACKEND_JIT, NULL, &jit);
    }
    char diff[64] = {0};
    bool ok = interp_only ? (ri == HB_OK) :
              (ri == HB_OK && rj == HB_OK && snapshots_equal(&interp, &jit, diff, sizeof(diff)));
    printf("{\"ok\":%s,\"arch\":\"%s\",\"seed\":\"0x%016" PRIx64 "\",\"code\":\"",
           ok ? "true" : "false", arch == HB_ARCH_X86 ? "x86" : "x64", seed);
    print_hex_bytes(code, code_len);
    printf("\",\"diff\":\"%s\",", diff);
    print_snapshot_json("initial", &initial);
    putchar(',');
    print_snapshot_json("interp", &interp);
    putchar(',');
    print_snapshot_json("jit", &jit);
    printf("}\n");
}

int main(void) {
    const char* arch_env = getenv("HB_DIFF_ARCH");
    hb_arch_t arch = (arch_env && strcmp(arch_env, "x86") == 0) ? HB_ARCH_X86 : HB_ARCH_X64;
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char* seed_s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        *p++ = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        char* code_s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        *p = 0;
        uint64_t seed = strtoull(seed_s, NULL, 0);
        run_case_line(arch, seed, code_s);
        fflush(stdout);
    }
    return 0;
}
