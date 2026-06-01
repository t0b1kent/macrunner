#include "hb_context.h"
#include "hb_decoder.h"
#include "hb_flags.h"
#include "hb_ir.h"
#include "hb_lifter.h"
#include "hb_memory.h"
#include "hb_runtime.h"
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

typedef struct {
    uint64_t gpr[16];
    uint64_t rip;
    hb_flags_t flags;
    uint64_t rflags;
    uint32_t flag_mask;
    hb_result_t flag_status;
    uint8_t xmm[16][16];
    uint8_t ymm_hi[16][16];
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

static bool parse_hex(const char* s, uint8_t* out, size_t* out_len) {
    size_t n = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        int hi = hex_nibble(*s++);
        if (hi < 0 || !*s) return false;
        int lo = hex_nibble(*s++);
        if (lo < 0 || n >= HB_DIFF_MAX_CODE) return false;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n;
    return n > 0;
}

static void print_hex_bytes(const uint8_t* bytes, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        putchar(hex[bytes[i] >> 4]);
        putchar(hex[bytes[i] & 0xf]);
    }
}

static void set_flags_from_bits(hb_context_t* ctx, uint64_t bits) {
    ctx->flags.cf = (bits & 0x001) != 0;
    ctx->flags.pf = (bits & 0x004) != 0;
    ctx->flags.af = (bits & 0x010) != 0;
    ctx->flags.zf = (bits & 0x040) != 0;
    ctx->flags.sf = (bits & 0x080) != 0;
    ctx->flags.of = (bits & 0x800) != 0;
    ctx->regs.x64.rflags = bits | 0x202ULL;
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
    r = hb_memory_write(ctx->memory, HB_DIFF_DATA_BASE, data, sizeof(data));
    if (r != HB_OK) return r;
    r = hb_memory_write(ctx->memory, HB_DIFF_STACK_BASE, stack, sizeof(stack));
    if (r != HB_OK) return r;

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
    ctx->pc = HB_DIFF_CODE_BASE;
    set_flags_from_bits(ctx, splitmix64_next(&rng));
    for (unsigned i = 0; i < 16; i++) {
        fill_random(&rng, (uint8_t*)ctx->regs.x64.xmm[i], 16);
        fill_random(&rng, (uint8_t*)ctx->ymm_hi[i], 16);
    }
    return HB_OK;
}

static hb_result_t lift_code(const uint8_t* code, size_t code_len, hb_ir_func_t** out_func) {
    hb_decoder_t* dec = hb_decoder_create(HB_ARCH_X64, code, code_len, HB_DIFF_CODE_BASE);
    if (!dec) return HB_ERR_OUT_OF_MEMORY;
    hb_result_t r = hb_lift_func_x64(dec, out_func);
    hb_decoder_destroy(dec);
    return r;
}

static void capture_snapshot(hb_context_t* ctx, hb_diff_snapshot_t* s,
                              hb_result_t api_result, hb_exec_result_t* exec) {
    memset(s, 0, sizeof(*s));
    s->flag_mask = ctx->lazy_flags.pending ? ctx->lazy_flags.valid_mask : HB_FLAG_BIT_ALL;
    s->flag_status = hb_lazy_flags_materialize(ctx, s->flag_mask);
    s->gpr[0] = ctx->regs.x64.rax; s->gpr[1] = ctx->regs.x64.rbx;
    s->gpr[2] = ctx->regs.x64.rcx; s->gpr[3] = ctx->regs.x64.rdx;
    s->gpr[4] = ctx->regs.x64.rsi; s->gpr[5] = ctx->regs.x64.rdi;
    s->gpr[6] = ctx->regs.x64.rsp; s->gpr[7] = ctx->regs.x64.rbp;
    s->gpr[8] = ctx->regs.x64.r8;  s->gpr[9] = ctx->regs.x64.r9;
    s->gpr[10] = ctx->regs.x64.r10; s->gpr[11] = ctx->regs.x64.r11;
    s->gpr[12] = ctx->regs.x64.r12; s->gpr[13] = ctx->regs.x64.r13;
    s->gpr[14] = ctx->regs.x64.r14; s->gpr[15] = ctx->regs.x64.r15;
    s->rip = ctx->pc;
    s->flags = ctx->flags;
    s->rflags = ctx->regs.x64.rflags;
    for (unsigned i = 0; i < 16; i++) {
        memcpy(s->xmm[i], ctx->regs.x64.xmm[i], 16);
        memcpy(s->ymm_hi[i], ctx->ymm_hi[i], 16);
    }
    if (ctx->memory) {
        (void)hb_memory_read(ctx->memory, HB_DIFF_DATA_BASE, s->data, sizeof(s->data));
        (void)hb_memory_read(ctx->memory, HB_DIFF_STACK_BASE, s->stack, sizeof(s->stack));
    }
    s->api_result = api_result;
    s->exec_result = exec ? exec->result : api_result;
}

static hb_result_t run_backend(const uint8_t* code, size_t code_len, uint64_t seed,
                               hb_backend_t backend, hb_diff_snapshot_t* initial,
                               hb_diff_snapshot_t* final) {
    hb_ir_func_t* func = NULL;
    hb_result_t r = lift_code(code, code_len, &func);
    if (r != HB_OK) return r;
    hb_context_t* ctx = hb_context_create(HB_ARCH_X64, backend);
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

static const char* reg_name(unsigned i) {
    static const char* names[16] = {
        "rax","rbx","rcx","rdx","rsi","rdi","rsp","rbp",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    return names[i];
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
    for (unsigned i = 0; i < 16; i++) {
        if (a->gpr[i] != b->gpr[i]) {
            snprintf(field, field_size, "reg:%s", reg_name(i));
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
    for (unsigned i = 0; i < 16; i++) {
        if (i) putchar(',');
        printf("\"%s\":\"0x%016" PRIx64 "\"", reg_name(i), s->gpr[i]);
    }
    printf(",\"rip\":\"0x%016" PRIx64 "\"}", s->rip);
}

static void print_flags_json(const hb_flags_t* f) {
    printf("{\"cf\":%u,\"pf\":%u,\"af\":%u,\"zf\":%u,\"sf\":%u,\"of\":%u}",
           f->cf ? 1 : 0, f->pf ? 1 : 0, f->af ? 1 : 0,
           f->zf ? 1 : 0, f->sf ? 1 : 0, f->of ? 1 : 0);
}

static void print_snapshot_json(const char* name, const hb_diff_snapshot_t* s) {
    printf("\"%s\":{\"api\":%d,\"result\":%d,\"regs\":", name, s->api_result, s->exec_result);
    print_regs_json(s);
    printf(",\"flags\":");
    print_flags_json(&s->flags);
    printf(",\"flag_mask\":\"0x%02x\",\"flag_status\":%d", s->flag_mask, s->flag_status);
    printf(",\"xmm0\":\"");
    print_hex_bytes(s->xmm[0], 16);
    printf("\",\"ymm0_hi\":\"");
    print_hex_bytes(s->ymm_hi[0], 16);
    printf("\"}");
}

static void run_case_line(uint64_t seed, const char* code_hex) {
    uint8_t code[HB_DIFF_MAX_CODE];
    size_t code_len = 0;
    if (!parse_hex(code_hex, code, &code_len)) {
        printf("{\"ok\":false,\"error\":\"bad_hex\"}\n");
        return;
    }
    hb_diff_snapshot_t initial, interp, jit;
    memset(&initial, 0, sizeof(initial));
    hb_result_t ri = run_backend(code, code_len, seed, HB_BACKEND_INTERP, &initial, &interp);
    hb_result_t rj = run_backend(code, code_len, seed, HB_BACKEND_JIT, NULL, &jit);
    char diff[64] = {0};
    bool ok = (ri == HB_OK && rj == HB_OK && snapshots_equal(&interp, &jit, diff, sizeof(diff)));
    printf("{\"ok\":%s,\"seed\":\"0x%016" PRIx64 "\",\"code\":\"", ok ? "true" : "false", seed);
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
        run_case_line(seed, code_s);
        fflush(stdout);
    }
    return 0;
}
