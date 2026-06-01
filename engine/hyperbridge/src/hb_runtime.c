#include "hb_runtime.h"
#include "hb_codegen.h"
#include "hb_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- In-memory block cache helpers --- */
static size_t block_cache_hash(uint64_t addr) {
    return (size_t)((addr ^ (addr >> 32)) & (HB_BLOCK_CACHE_SIZE - 1));
}

static hb_block_cache_t* block_cache_create(void) {
    return calloc(1, sizeof(hb_block_cache_t));
}

static void block_cache_destroy(hb_block_cache_t* cache) {
    free(cache);
}

static hb_block_cache_entry_t* block_cache_find(hb_block_cache_t* cache, uint64_t addr) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) return NULL;
        if (cache->entries[probe].guest_addr == addr) return &cache->entries[probe];
    }
    return NULL;
}

static hb_block_cache_entry_t* block_cache_put(hb_block_cache_t* cache, uint64_t addr, uint8_t* code,
                                               size_t size, uint32_t steps,
                                               const hb_ir_block_t* block, bool fused) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) {
            cache->entries[probe].guest_addr = addr;
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            cache->entries[probe].hit_count = 0;
            cache->entries[probe].block = block;
            cache->entries[probe].fused = fused;
            cache->entries[probe].valid = true;
            return &cache->entries[probe];
        }
        if (cache->entries[probe].guest_addr == addr) {
            /* Update existing entry */
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            cache->entries[probe].block = block;
            cache->entries[probe].fused = fused;
            return &cache->entries[probe];
        }
    }
    return NULL;
}

static int trace_jit_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_native_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static int trace_jit_blocks_budget_allows(int force) {
    static int count;
    static int exhausted;
    const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCK_BUDGET");
    int limit = env && *env ? atoi(env) : 2000;
    if (!trace_jit_blocks_enabled()) return 0;
    if (force || limit <= 0) return 1;
    if (count < limit) {
        count++;
        return 1;
    }
    if (!exhausted) {
        exhausted = 1;
        fprintf(stderr, "macrunner-hb-jit-block: trace budget exhausted at %d entries, silencing\n", limit);
        fflush(stderr);
    }
    return 0;
}

static void trace_jit_block(uint64_t guest_pc, const uint8_t* native, size_t native_size,
                            const hb_ir_block_t* block) {
    uint64_t watch = trace_jit_native_addr();
    uint64_t guest_watch = trace_jit_guest_addr();
    int matched = watch && (uintptr_t)native <= (uintptr_t)watch &&
                  (uintptr_t)watch < (uintptr_t)native + native_size;
    int guest_matched = guest_watch && guest_pc == guest_watch;
    const hb_ir_instr_t* first = (block && block->instr_count) ? &block->instrs[0] : NULL;
    const hb_ir_instr_t* last = (block && block->instr_count) ?
                                &block->instrs[block->instr_count - 1] : NULL;

    if (!guest_matched && guest_watch && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            uint64_t start = instr->guest_addr;
            uint64_t end = start + instr->guest_len;
            if (guest_watch == start || (instr->guest_len && guest_watch >= start && guest_watch < end)) {
                guest_matched = 1;
                break;
            }
        }
    }
    matched = matched || guest_matched;
    if (!trace_jit_blocks_budget_allows(matched)) return;
    fprintf(stderr, "macrunner-hb-jit-block: guest=%p native=%p-%p size=%zu instrs=%zu "
            "first_op=%u first_guest=%p last_op=%u last_guest=%p last_target=%p%s\n",
            (void*)(uintptr_t)guest_pc, native, native + native_size, native_size,
            block ? block->instr_count : 0,
            first ? (unsigned)first->op : 0, first ? (void*)(uintptr_t)first->guest_addr : NULL,
            last ? (unsigned)last->op : 0, last ? (void*)(uintptr_t)last->guest_addr : NULL,
            last ? (void*)(uintptr_t)last->target : NULL, matched ? " match=1" : "");
    if (matched && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            fprintf(stderr, "macrunner-hb-jit-block-ir: guest=%p op=%u target=%p len=%u\n",
                    (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->op,
                    (void*)(uintptr_t)instr->target, (unsigned)instr->guest_len);
            fprintf(stderr,
                    "macrunner-hb-jit-block-ir-operands: guest=%p "
                    "dst{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src1{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src2{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} cc=%u\n",
                    (void*)(uintptr_t)instr->guest_addr,
                    (unsigned)instr->dst.type, (unsigned)instr->dst.size,
                    (unsigned)instr->dst.reg, (unsigned)instr->dst.reg_offset,
                    (long long)instr->dst.imm, (unsigned)instr->dst.mem.base,
                    (unsigned)instr->dst.mem.index, (unsigned)instr->dst.mem.scale,
                    (long long)instr->dst.mem.disp, (unsigned)instr->dst.mem.segment,
                    (unsigned)instr->dst.mem.addr32,
                    (unsigned)instr->src1.type, (unsigned)instr->src1.size,
                    (unsigned)instr->src1.reg, (unsigned)instr->src1.reg_offset,
                    (long long)instr->src1.imm, (unsigned)instr->src1.mem.base,
                    (unsigned)instr->src1.mem.index, (unsigned)instr->src1.mem.scale,
                    (long long)instr->src1.mem.disp, (unsigned)instr->src1.mem.segment,
                    (unsigned)instr->src1.mem.addr32,
                    (unsigned)instr->src2.type, (unsigned)instr->src2.size,
                    (unsigned)instr->src2.reg, (unsigned)instr->src2.reg_offset,
                    (long long)instr->src2.imm, (unsigned)instr->src2.mem.base,
                    (unsigned)instr->src2.mem.index, (unsigned)instr->src2.mem.scale,
                    (long long)instr->src2.mem.disp, (unsigned)instr->src2.mem.segment,
                    (unsigned)instr->src2.mem.addr32,
                    (unsigned)instr->cc);
        }
    }
    fflush(stderr);
}

static int trace_jit_hot_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_hot_interval(void) {
    static int parsed;
    static uint64_t interval;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCK_INTERVAL");
        interval = env && *env ? strtoull(env, NULL, 0) : 500000ULL;
        if (interval < 1000ULL) interval = 1000ULL;
        parsed = 1;
    }
    return interval;
}

static int trace_jit_hot_bytes_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static size_t trace_jit_hot_bytes_len(void) {
    static int parsed;
    static size_t len;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES_LEN");
        len = env && *env ? (size_t)strtoull(env, NULL, 0) : 16;
        if (len < 1) len = 16;
        if (len > 128) len = 128;
        parsed = 1;
    }
    return len;
}

static void trace_jit_hot_guest_bytes(hb_context_t* ctx, uint64_t guest_addr) {
    uint8_t byte;
    size_t len;
    if (!trace_jit_hot_bytes_enabled() || !ctx || !ctx->memory) return;
    len = trace_jit_hot_bytes_len();
    fprintf(stderr, " bytes=");
    for (size_t i = 0; i < len; i++) {
        if (hb_memory_read_u8(ctx->memory, (hb_gva_t)(guest_addr + i), &byte) != HB_OK) {
            fprintf(stderr, "%s??", i ? " " : "");
            break;
        }
        fprintf(stderr, "%s%02x", i ? " " : "", byte);
    }
}

static void trace_jit_hot_block_tick(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry) {
    const size_t top_count = 12;
    hb_block_cache_entry_t* top[12] = {0};

    if (!rt || !entry || !trace_jit_hot_blocks_enabled()) return;
    entry->hit_count++;
    rt->hot_trace_blocks++;
    if (!rt->hot_trace_next)
        rt->hot_trace_next = trace_jit_hot_interval();
    if (rt->hot_trace_blocks < rt->hot_trace_next) return;
    rt->hot_trace_next += trace_jit_hot_interval();

    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        hb_block_cache_entry_t* candidate = &rt->block_cache->entries[i];
        if (!candidate->valid || !candidate->hit_count) continue;
        for (size_t j = 0; j < top_count; j++) {
            if (!top[j] || candidate->hit_count > top[j]->hit_count) {
                for (size_t k = top_count - 1; k > j; k--) top[k] = top[k - 1];
                top[j] = candidate;
                break;
            }
        }
    }

    fprintf(stderr, "macrunner-hb-jit-hot-blocks: total=%llu interval=%llu used=%zu\n",
            (unsigned long long)rt->hot_trace_blocks,
            (unsigned long long)trace_jit_hot_interval(),
            rt->jit_mem ? rt->jit_mem->used : 0);
    for (size_t i = 0; i < top_count && top[i]; i++) {
        fprintf(stderr, "macrunner-hb-jit-hot-block: rank=%zu guest=%p hits=%llu native=%p "
                "size=%zu steps=%u",
                i + 1, (void*)(uintptr_t)top[i]->guest_addr,
                (unsigned long long)top[i]->hit_count,
                top[i]->native_code, top[i]->native_size, top[i]->steps);
        trace_jit_hot_guest_bytes(rt->ctx, top[i]->guest_addr);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx) {
    hb_jit_runtime_t* rt = calloc(1, sizeof(hb_jit_runtime_t));
    const char* size_env;
    size_t jit_size = 128u * 1024u * 1024u;
    if (!rt) return NULL;
    rt->ctx = ctx;
    size_env = getenv("MACRUNNER_HB_JIT_BUFFER_SIZE");
    if (size_env && *size_env) {
        unsigned long long parsed = strtoull(size_env, NULL, 0);
        if (parsed >= 65536ULL && parsed <= 512ULL * 1024ULL * 1024ULL)
            jit_size = (size_t)parsed;
    }
    rt->jit_mem = hb_jit_buffer_create(jit_size);
    if (!rt->jit_mem) { free(rt); return NULL; }
    rt->block_cache = block_cache_create();
    if (!rt->block_cache) {
        hb_jit_buffer_destroy(rt->jit_mem);
        free(rt);
        return NULL;
    }
    return rt;
}

void hb_jit_runtime_destroy(hb_jit_runtime_t* rt) {
    if (!rt) return;
    hb_jit_buffer_destroy(rt->jit_mem);
    block_cache_destroy(rt->block_cache);
    free(rt);
}

/* Find block by guest address */
static hb_ir_block_t* find_block(const hb_ir_cfg_t* cfg, uint64_t addr) {
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

static bool is_control_transfer_op(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

static const hb_ir_instr_t* first_control_transfer_instr(const hb_ir_block_t* block) {
    if (!block) return NULL;
    for (size_t i = 0; i < block->instr_count; i++) {
        if (is_control_transfer_op(block->instrs[i].op)) return &block->instrs[i];
    }
    return NULL;
}

static uint32_t jit_block_step_count(const hb_ir_block_t* block) {
    const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
    if (!block) return 0;
    if (!transfer) return (uint32_t)block->instr_count;
    return (uint32_t)((size_t)(transfer - block->instrs) + 1);
}

static void sync_arch_pc_after_jit_block(hb_context_t* ctx) {
    if (!ctx) return;
    if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
    else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
}

static void set_helper_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                    uint64_t steps, uint64_t blocks_executed) {
    out->result = ctx->last_result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = "JIT helper fault";
}

static hb_result_t set_runtime_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                            hb_result_t result, uint64_t steps,
                                            uint64_t blocks_executed,
                                            const char* reason) {
    if (ctx) ctx->last_result = result;
    out->result = result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = reason;
    return result;
}

static void try_promote_copy_scan_counted_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                               const hb_ir_block_t* block) {
    const hb_ir_block_t* body = NULL;
    const hb_ir_block_t* guard = NULL;
    hb_block_cache_entry_t* body_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        body_entry = block_cache_find(rt->block_cache, last->target);
        if (!body_entry || !body_entry->block || body_entry->fused) return;
        body = body_entry->block;
        guard = block;
    } else if (block->instr_count == 5 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* guard_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        if (!guard_entry || !guard_entry->block) return;
        body_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || body_entry->fused) return;
        body = block;
        guard = guard_entry->block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_copy_scan_counted_loop(cg, body, guard, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, body->guest_addr, dest, emitted_size,
                    (uint32_t)(body->instr_count + guard->instr_count), body, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=copy-scan-counted body=%p guard=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)body->guest_addr, (void*)(uintptr_t)guard->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static void try_promote_bounded_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* guard = NULL;
    const hb_ir_block_t* body = NULL;
    hb_block_cache_entry_t* guard_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* body_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        guard_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || !body_entry->block || !guard_entry || guard_entry->fused) return;
        guard = block;
        body = body_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        guard_entry = block_cache_find(rt->block_cache, last->target);
        if (!guard_entry || !guard_entry->block || guard_entry->fused) return;
        guard = guard_entry->block;
        body = block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_bounded_scan_loop(cg, guard, body, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, guard->guest_addr, dest, emitted_size,
                    (uint32_t)(guard->instr_count + body->instr_count), guard, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=bounded-byte-scan guard=%p body=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)guard->guest_addr, (void*)(uintptr_t)body->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool same_plain_runtime_reg_operand(const hb_ir_operand_t* a,
                                           const hb_ir_operand_t* b) {
    return a && b &&
           a->type == HB_OP_REG && b->type == HB_OP_REG &&
           a->reg == b->reg && a->size == b->size &&
           a->reg_offset == b->reg_offset;
}

static bool zero_extend_mem8_to_reg32(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ZERO_EXTEND &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_32 &&
           instr->src1.type == HB_OP_MEM && instr->src1.size == HB_SIZE_8;
}

static bool add_imm1_same_reg64(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ADD &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, &instr->src1) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == 1;
}

static bool test_same_reg32(const hb_ir_instr_t* instr, const hb_ir_operand_t* reg) {
    return instr && reg && instr->op == HB_IR_TEST &&
           instr->src1.type == HB_OP_REG && instr->src1.size == HB_SIZE_32 &&
           same_plain_runtime_reg_operand(&instr->src1, &instr->src2) &&
           same_plain_runtime_reg_operand(&instr->src1, reg);
}

static bool byte_compare_loop_pair(const hb_ir_block_t* cmp_block,
                                   const hb_ir_block_t* backedge_block) {
    const hb_ir_instr_t *lhs, *rhs, *sub, *cmp_jcc;
    const hb_ir_instr_t *inc, *test, *back_jcc;
    if (!cmp_block || !backedge_block ||
        cmp_block->instr_count != 4 || backedge_block->instr_count != 3)
        return false;

    lhs = &cmp_block->instrs[0];
    rhs = &cmp_block->instrs[1];
    sub = &cmp_block->instrs[2];
    cmp_jcc = &cmp_block->instrs[3];
    inc = &backedge_block->instrs[0];
    test = &backedge_block->instrs[1];
    back_jcc = &backedge_block->instrs[2];

    if (!zero_extend_mem8_to_reg32(lhs) || !zero_extend_mem8_to_reg32(rhs))
        return false;
    if (sub->op != HB_IR_SUB ||
        !same_plain_runtime_reg_operand(&sub->dst, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src1, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src2, &rhs->dst))
        return false;
    if (cmp_jcc->op != HB_IR_Jcc ||
        (cmp_jcc->cc != HB_CC_E && cmp_jcc->cc != HB_CC_NE) ||
        cmp_jcc->guest_addr + cmp_jcc->guest_len != backedge_block->guest_addr)
        return false;
    if (!add_imm1_same_reg64(inc) ||
        !test_same_reg32(test, &rhs->dst) ||
        back_jcc->op != HB_IR_Jcc ||
        (back_jcc->cc != HB_CC_E && back_jcc->cc != HB_CC_NE) ||
        back_jcc->target != cmp_block->guest_addr)
        return false;
    return true;
}

static void try_promote_byte_compare_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* cmp_block = NULL;
    const hb_ir_block_t* backedge_block = NULL;
    hb_block_cache_entry_t* cmp_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 4 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* backedge_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        cmp_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!backedge_entry || !backedge_entry->block || !cmp_entry || cmp_entry->fused) return;
        cmp_block = block;
        backedge_block = backedge_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        cmp_entry = block_cache_find(rt->block_cache, last->target);
        if (!cmp_entry || !cmp_entry->block || cmp_entry->fused) return;
        cmp_block = cmp_entry->block;
        backedge_block = block;
    } else {
        return;
    }

    if (!byte_compare_loop_pair(cmp_block, backedge_block)) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, cmp_block, backedge_block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, cmp_block->guest_addr, dest, emitted_size,
                    (uint32_t)(cmp_block->instr_count + backedge_block->instr_count),
                    cmp_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=byte-compare-loop cmp=%p backedge=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)cmp_block->guest_addr,
                (void*)(uintptr_t)backedge_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool sub_imm_same_reg64(const hb_ir_instr_t* instr,
                               const hb_ir_operand_t* reg,
                               int64_t imm) {
    return instr && reg && instr->op == HB_IR_SUB &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, reg) &&
           same_plain_runtime_reg_operand(&instr->src1, reg) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == imm;
}

static bool load_test_nonzero_qword_block(const hb_ir_block_t* block,
                                          hb_ir_operand_t* index_reg,
                                          uint64_t* fallthrough,
                                          uint64_t* nonzero_target) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* test = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_64 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_64 ||
        load->src1.mem.index >= HB_REG_COUNT || load->src1.mem.scale != 8)
        return false;
    if (test->src1.type != HB_OP_REG || test->src1.size != HB_SIZE_64 ||
        !same_plain_runtime_reg_operand(&test->src1, &test->src2) ||
        !same_plain_runtime_reg_operand(&test->src1, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (index_reg) *index_reg = hb_ir_reg(load->src1.mem.index, HB_SIZE_64);
    if (fallthrough) *fallthrough = jcc->guest_addr + jcc->guest_len;
    if (nonzero_target) *nonzero_target = jcc->target;
    return true;
}

static bool dec_to_test_block(const hb_ir_block_t* block,
                              const hb_ir_operand_t* index_reg,
                              uint64_t* test_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* sub = &block->instrs[0];
    const hb_ir_instr_t* jmp = &block->instrs[1];
    if (!sub_imm_same_reg64(sub, index_reg, 1)) return false;
    if (jmp->op != HB_IR_JMP) return false;
    if (test_target) *test_target = jmp->target;
    return true;
}

static bool test_nonnegative_backedge_block(const hb_ir_block_t* block,
                                            const hb_ir_operand_t* index_reg,
                                            uint64_t load_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* test = &block->instrs[0];
    const hb_ir_instr_t* jcc = &block->instrs[1];
    return test && test->op == HB_IR_TEST &&
           same_plain_runtime_reg_operand(&test->src1, index_reg) &&
           same_plain_runtime_reg_operand(&test->src2, index_reg) &&
           jcc->op == HB_IR_Jcc && jcc->cc == HB_CC_NS && jcc->target == load_target;
}

static void try_promote_null_qword_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                             const hb_ir_block_t* block) {
    const hb_ir_block_t* load_block = NULL;
    const hb_ir_block_t* dec_block = NULL;
    const hb_ir_block_t* test_block = NULL;
    hb_block_cache_entry_t* load_entry = NULL;
    hb_ir_operand_t index_reg = hb_ir_none();
    uint64_t dec_addr = 0;
    uint64_t test_addr = 0;

    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !block)
        return;

    if (load_test_nonzero_qword_block(block, &index_reg, &dec_addr, NULL)) {
        load_block = block;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr))
            return;
        hb_block_cache_entry_t* test_entry = block_cache_find(rt->block_cache, test_addr);
        if (!test_entry || !test_entry->block) return;
        dec_block = dec_entry->block;
        test_block = test_entry->block;
        load_entry = block_cache_find(rt->block_cache, load_block->guest_addr);
    } else if (block->instr_count == 2 && block->instrs[1].op == HB_IR_Jcc) {
        test_block = block;
        uint64_t load_addr = block->instrs[1].target;
        load_entry = block_cache_find(rt->block_cache, load_addr);
        if (!load_entry || !load_entry->block ||
            !load_test_nonzero_qword_block(load_entry->block, &index_reg, &dec_addr, NULL))
            return;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr) ||
            test_addr != test_block->guest_addr)
            return;
        load_block = load_entry->block;
        dec_block = dec_entry->block;
    } else {
        return;
    }

    if (!load_entry || load_entry->fused ||
        !test_nonnegative_backedge_block(test_block, &index_reg, load_block->guest_addr))
        return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_four_block_loop_helper(cg, load_block, dec_block,
                                                            test_block, NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, load_block->guest_addr, dest, emitted_size,
                    (uint32_t)(load_block->instr_count + dec_block->instr_count +
                               test_block->instr_count),
                    load_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=null-qword-scan load=%p dec=%p test=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)load_block->guest_addr,
                (void*)(uintptr_t)dec_block->guest_addr,
                (void*)(uintptr_t)test_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool load_cmp_jne_i32_entry_block(const hb_ir_block_t* block,
                                         uint64_t* equal_addr,
                                         uint64_t* less_addr) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* cmp = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_32 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_32)
        return false;
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_MEM || cmp->src1.size != HB_SIZE_32 ||
        !same_plain_runtime_reg_operand(&cmp->src2, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (equal_addr) *equal_addr = jcc->guest_addr + jcc->guest_len;
    if (less_addr) *less_addr = jcc->target;
    return true;
}

static bool cmp_setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                                hb_ir_operand_t* setcc_dst) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* cmp = &block->instrs[0];
    const hb_ir_instr_t* setcc = &block->instrs[1];
    const hb_ir_instr_t* ret = &block->instrs[2];
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_REG || cmp->src1.size != HB_SIZE_64 ||
        cmp->src2.type != HB_OP_REG || cmp->src2.size != HB_SIZE_64)
        return false;
    if (setcc->op != HB_IR_SETcc || setcc->cc != cc || setcc->dst.size != HB_SIZE_8 ||
        ret->op != HB_IR_RET)
        return false;
    if (setcc_dst) *setcc_dst = setcc->dst;
    return true;
}

static bool setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                            const hb_ir_operand_t* expected_dst) {
    if (!block || !expected_dst || block->instr_count != 2) return false;
    const hb_ir_instr_t* setcc = &block->instrs[0];
    const hb_ir_instr_t* ret = &block->instrs[1];
    return setcc->op == HB_IR_SETcc && setcc->cc == cc &&
           same_plain_runtime_reg_operand(&setcc->dst, expected_dst) &&
           ret->op == HB_IR_RET;
}

static const hb_ir_block_t* find_comparator_entry_pred(const hb_ir_block_t* block,
                                                       uint64_t* equal_addr,
                                                       uint64_t* less_addr) {
    if (load_cmp_jne_i32_entry_block(block, equal_addr, less_addr))
        return block;
    if (!block) return NULL;
    for (size_t i = 0; i < block->pred_count; i++) {
        const hb_ir_block_t* pred = block->pred[i];
        if (load_cmp_jne_i32_entry_block(pred, equal_addr, less_addr))
            return pred;
    }
    return NULL;
}

static const hb_ir_block_t* find_comparator_entry_near_cache(hb_block_cache_t* cache,
                                                             const hb_ir_block_t* equal_block,
                                                             uint64_t* equal_addr,
                                                             uint64_t* less_addr) {
    hb_ir_operand_t ignored = hb_ir_none();
    if (!cache || !equal_block || !cmp_setcc_ret_block(equal_block, HB_CC_B, &ignored))
        return NULL;
    for (uint64_t back = 1; back <= 16; back++) {
        hb_block_cache_entry_t* entry = block_cache_find(cache, equal_block->guest_addr - back);
        if (!entry || !entry->block) continue;
        uint64_t candidate_equal = 0;
        uint64_t candidate_less = 0;
        if (load_cmp_jne_i32_entry_block(entry->block, &candidate_equal, &candidate_less) &&
            candidate_equal == equal_block->guest_addr) {
            if (equal_addr) *equal_addr = candidate_equal;
            if (less_addr) *less_addr = candidate_less;
            return entry->block;
        }
    }
    return NULL;
}

static void try_promote_i32_less_tiebreaker_comparator(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                                       const hb_ir_block_t* block) {
    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !block)
        return;

    uint64_t equal_addr = 0;
    uint64_t less_addr = 0;
    const hb_ir_block_t* entry_block = find_comparator_entry_pred(block, &equal_addr, &less_addr);
    if (!entry_block)
        entry_block = find_comparator_entry_near_cache(rt->block_cache, block, &equal_addr, &less_addr);
    if (!entry_block) return;

    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, entry_block->guest_addr);
    hb_block_cache_entry_t* equal = block_cache_find(rt->block_cache, equal_addr);
    hb_block_cache_entry_t* less = block_cache_find(rt->block_cache, less_addr);
    if (!entry || entry->fused || !equal || !equal->block)
        return;

    hb_ir_operand_t setcc_dst = hb_ir_none();
    if (!cmp_setcc_ret_block(equal->block, HB_CC_B, &setcc_dst))
        return;
    if (less && less->block && !setcc_ret_block(less->block, HB_CC_L, &setcc_dst))
        less = NULL;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_i32_less_tiebreaker_helper(
        cg, entry_block, equal->block, less && less->block ? less->block : NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, entry_block->guest_addr, dest, emitted_size,
                    (uint32_t)(entry_block->instr_count + equal->block->instr_count +
                               (less && less->block ? less->block->instr_count : 0)),
                    entry_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=i32-less-tiebreaker entry=%p equal=%p less=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)entry_block->guest_addr,
                (void*)(uintptr_t)equal->block->guest_addr,
                (void*)(uintptr_t)(less && less->block ? less->block->guest_addr : 0),
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool small_terminal_jcc_self_loop(const hb_ir_block_t* block) {
    if (!block || block->instr_count < 2 || block->instr_count > 16) return false;
    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (last->op != HB_IR_Jcc || last->target != block->guest_addr) return false;
    for (size_t i = 0; i + 1 < block->instr_count; i++) {
        hb_ir_op_t op = block->instrs[i].op;
        if (op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
            op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ)
            return false;
    }
    return true;
}

static void try_promote_self_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                  const hb_ir_block_t* block) {
    if (!rt || !rt->block_cache || !rt->jit_mem || !ctx || !small_terminal_jcc_self_loop(block))
        return;
    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, block->guest_addr);
    if (!entry || entry->fused) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, block, block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, block->guest_addr, dest, emitted_size,
                    (uint32_t)block->instr_count, block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=self-loop block=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)block->guest_addr, dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

hb_result_t hb_jit_runtime_compile(hb_jit_runtime_t* rt, const hb_ir_func_t* func) {
    (void)rt; (void)func;
    /* Compilation is done on-demand per-block in hb_jit_runtime_run for MVP */
    return HB_OK;
}

hb_result_t hb_jit_runtime_run(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out) {
    if (!rt || !func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = rt->ctx;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;
    ctx->last_result = HB_OK;

    while (1) {
        if (ctx->step_limit > 0 && steps >= ctx->step_limit) {
            out->result = HB_ERR_STEP_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            return set_runtime_fault_result(out, ctx, HB_ERR_BLOCK_LIMIT, steps,
                                            blocks_executed, "block limit reached");
        }

        hb_ir_block_t* block = find_block(func->cfg, ctx->pc);
        if (!block) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before current PC");
            }
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK; /* No block for PC — function exit or external call */
        }

        blocks_executed++;

        /* Check in-memory block cache */
        hb_block_cache_entry_t* cached = block_cache_find(rt->block_cache, ctx->pc);
        if (cached) {
            typedef void (*jit_block_t)(hb_context_t*);
            jit_block_t exec = (jit_block_t)(void*)cached->native_code;
            exec(ctx);
            trace_jit_hot_block_tick(rt, cached);
            steps += cached->steps;
        } else {
            /* Compile block into codegen buffer */
            hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(4096);
            if (!code_buf) return HB_ERR_OUT_OF_MEMORY;

            hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
            if (!cg) { hb_codegen_buffer_destroy(code_buf); return HB_ERR_OUT_OF_MEMORY; }

            hb_result_t r = hb_arm64_codegen_block_with_cfg(cg, block, func->cfg, code_buf);
            hb_arm64_codegen_destroy(cg);
            if (r != HB_OK) {
                hb_codegen_buffer_destroy(code_buf);
                out->result = r;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                out->faulted = true;
                out->fault_reason = "JIT codegen failed";
                return HB_OK;
            }

            /* Append to JIT buffer (bump allocator) */
            r = hb_jit_buffer_make_writable(rt->jit_mem);
            if (r != HB_OK) { hb_codegen_buffer_destroy(code_buf); return r; }

            size_t needed = code_buf->size;
            if (rt->jit_mem->used + needed > rt->jit_mem->size) {
                hb_codegen_buffer_destroy(code_buf);
                out->result = HB_ERR_OUT_OF_MEMORY;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                out->fault_reason = "JIT buffer exhausted";
                return HB_OK;
            }

            size_t emitted_size = code_buf->size;
            uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
            memcpy(dest, code_buf->code, emitted_size);
            rt->jit_mem->used += emitted_size;
            /* Align to 16-byte boundary for next block */
            rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
            hb_codegen_buffer_destroy(code_buf);

            r = hb_jit_buffer_commit(rt->jit_mem);
            if (r != HB_OK) return r;

            /* Store in block cache */
            cached = block_cache_put(rt->block_cache, ctx->pc, dest, emitted_size,
                                     jit_block_step_count(block), block, false);
            trace_jit_block(ctx->pc, dest, emitted_size, block);
            try_promote_copy_scan_counted_loop(rt, ctx, block);
            try_promote_bounded_scan_loop(rt, ctx, block);
            try_promote_byte_compare_loop(rt, ctx, block);
            try_promote_null_qword_scan_loop(rt, ctx, block);
            try_promote_i32_less_tiebreaker_comparator(rt, ctx, block);
            try_promote_self_loop(rt, ctx, block);

            /* Execute */
            typedef void (*jit_block_t)(hb_context_t*);
            jit_block_t exec = (jit_block_t)(void*)dest;
            exec(ctx);
            trace_jit_hot_block_tick(rt, cached);
            steps += jit_block_step_count(block);
        }
        sync_arch_pc_after_jit_block(ctx);
        if (ctx->last_result != HB_OK) {
            set_helper_fault_result(out, ctx, steps, blocks_executed);
            return HB_OK;
        }

        /* Determine if we should continue or stop */
        if (block->instr_count == 0) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
        const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
        const hb_ir_instr_t* terminal = transfer ? transfer : last;
        if (terminal->op == HB_IR_RET) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        /* For CALL/JMP/Jcc, PC was updated by JIT code; find next block */
        hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
        if (!next) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before branch target");
            }
            if (is_control_transfer_op(terminal->op)) {
                out->result = HB_OK;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK; /* External branch/call/return boundary */
            }
            out->result = HB_ERR_NOT_FOUND;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            out->faulted = true;
            out->fault_reason = "branch target block not found";
            return HB_OK;
        }
        if (terminal->op == HB_IR_JMP || terminal->op == HB_IR_Jcc ||
            terminal->op == HB_IR_CALL || terminal->op == HB_IR_LOOP ||
            terminal->op == HB_IR_JRCXZ) {
            /* Continue with the target block */
            continue;
        }

        /* Sequential block end — stop */
        ctx->pc = last->guest_addr + last->guest_len;
        if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
        else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
        next = find_block(func->cfg, ctx->pc);
        if (next) continue;
        out->result = HB_OK;
        out->steps_executed = steps;
        out->blocks_executed = blocks_executed;
        return HB_OK;
    }
}

hb_result_t hb_runtime_run(hb_context_t* ctx, const hb_ir_func_t* func, hb_backend_t backend, hb_exec_result_t* out) {
    if (!ctx || !func || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));
    switch (backend) {
        case HB_BACKEND_INTERP: {
            hb_interpreter_t* i = hb_interpreter_create(ctx);
            if (!i) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_interpreter_run(i, func, out);
            hb_interpreter_destroy(i);
            return r;
        }
        case HB_BACKEND_JIT:
        case HB_BACKEND_AOT: {
            hb_jit_runtime_t* rt = hb_jit_runtime_create(ctx);
            if (!rt) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_jit_runtime_run(rt, func, out);
            hb_jit_runtime_destroy(rt);
            return r;
        }
    }
    return HB_ERR_INVALID_ARG;
}
