#include "hb_ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hb_alloc_count.h"

/* --- IR Function --- */
hb_ir_func_t* hb_ir_func_create(uint64_t guest_addr, size_t guest_len) {
    hb_ir_func_t* func = calloc(1, sizeof(hb_ir_func_t));
    if (!func) return NULL;
    func->guest_addr = guest_addr;
    func->guest_len = guest_len;
    func->cfg = hb_ir_cfg_create();
    if (!func->cfg) {
        free(func);
        return NULL;
    }
    return func;
}

void hb_ir_func_destroy(hb_ir_func_t* func) {
    if (!func) return;
    if (func->cfg) hb_ir_cfg_destroy(func->cfg);
    if (func->flat_instrs) {
        free(func->flat_instrs);
    }
    free((void*)func->unsupported_reason);
    free(func);
}

/* --- IR Block --- */
/* MacRunner 2026-08-06 — учёт блоков ИР.
 *
 * Зачем. Разбор кучи живого процесса (heap PID) на 90-й секунде прогона HK показал
 * 73 576 196 ЖИВЫХ выделений на 22.6 ГБ, средний размер 307.7 байт, все "non-object",
 * то есть обычный malloc нашего кода. Скорость роста — около 800 тысяч выделений в
 * секунду. Физический след процесса 7.3 ГБ за 31 секунду, подкачка раздувалась до 68 ГБ,
 * свободное место на диске падало с 80 до 18 ГБ.
 *
 * hb_ir_block_create делает ДВА выделения на блок: сам блок (80 байт) и массив из 16
 * инструкций (16 * 184 = 2944 байта). Если создания не уравновешены уничтожениями,
 * это и есть источник. Счётчики отвечают на вопрос однозначно: разница между ними —
 * количество утёкших блоков, а не предположение о нём. */
unsigned long long hb_ir_blocks_created;
unsigned long long hb_ir_blocks_destroyed;

hb_ir_block_t* hb_ir_block_create(uint64_t id, uint64_t guest_addr) {
    hb_ir_block_t* block = calloc(1, sizeof(hb_ir_block_t));
    if (!block) return NULL;
    block->id = id;
    block->guest_addr = guest_addr;
    /* calloc would leave this 0, which is a VALID index and would claim instruction 0 is a
     * control transfer. Set the sentinel explicitly. */
    block->first_transfer_idx = HB_IR_TRANSFER_UNCOMPUTED;
    block->instr_cap = 16;
    block->instrs = calloc(block->instr_cap, sizeof(hb_ir_instr_t));
    if (!block->instrs) {
        free(block);
        return NULL;
    }
    __atomic_add_fetch(&hb_ir_blocks_created, 1, __ATOMIC_RELAXED);
    return block;
}

void hb_ir_block_destroy(hb_ir_block_t* block) {
    if (block) __atomic_add_fetch(&hb_ir_blocks_destroyed, 1, __ATOMIC_RELAXED);
    if (!block) return;
    free(block->instrs);
    free(block->succ);
    free(block->pred);
    free(block);
}

/* --- IR CFG --- */
hb_ir_cfg_t* hb_ir_cfg_create(void) {
    hb_ir_cfg_t* cfg = calloc(1, sizeof(hb_ir_cfg_t));
    if (!cfg) return NULL;
    cfg->block_cap = 8;
    cfg->blocks = calloc(cfg->block_cap, sizeof(hb_ir_block_t*));
    if (!cfg->blocks) {
        free(cfg);
        return NULL;
    }
    return cfg;
}

void hb_ir_cfg_destroy(hb_ir_cfg_t* cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->block_count; i++) {
        hb_ir_block_destroy(cfg->blocks[i]);
    }
    free(cfg->blocks);
    free(cfg);
}

void hb_ir_cfg_add_block(hb_ir_cfg_t* cfg, hb_ir_block_t* block) {
    if (!cfg || !block) return;
    if (cfg->block_count >= cfg->block_cap) {
        size_t new_cap = cfg->block_cap * 2;
        hb_ir_block_t** new_blocks = realloc(cfg->blocks, new_cap * sizeof(hb_ir_block_t*));
        if (!new_blocks) return;
        cfg->blocks = new_blocks;
        cfg->block_cap = new_cap;
    }
    cfg->blocks[cfg->block_count++] = block;
}

void hb_ir_cfg_add_edge(hb_ir_cfg_t* cfg, hb_ir_block_t* from, hb_ir_block_t* to) {
    if (!cfg || !from || !to) return;
    /* Add to succ */
    from->succ = realloc(from->succ, (from->succ_count + 1) * sizeof(hb_ir_block_t*));
    from->succ[from->succ_count++] = to;
    /* Add to pred */
    to->pred = realloc(to->pred, (to->pred_count + 1) * sizeof(hb_ir_block_t*));
    to->pred[to->pred_count++] = from;
}

/* --- IR Builder --- */
hb_ir_builder_t* hb_ir_builder_create(hb_ir_func_t* func) {
    hb_ir_builder_t* b = calloc(1, sizeof(hb_ir_builder_t));
    if (!b) return NULL;
    b->func = func;
    if (func->cfg && func->cfg->block_count > 0) {
        b->current_block = func->cfg->blocks[0];
    }
    return b;
}

void hb_ir_builder_destroy(hb_ir_builder_t* b) {
    free(b);
}

void hb_ir_builder_set_block(hb_ir_builder_t* b, hb_ir_block_t* block) {
    if (b) b->current_block = block;
}

hb_ir_instr_t* hb_ir_emit(hb_ir_builder_t* b, hb_ir_op_t op) {
    if (!b || !b->current_block) return NULL;
    hb_ir_block_t* blk = b->current_block;
    if (blk->instr_count >= blk->instr_cap) {
        size_t new_cap = blk->instr_cap * 2;
        hb_ir_instr_t* new_instrs = realloc(blk->instrs, new_cap * sizeof(hb_ir_instr_t));
        if (!new_instrs) return NULL;
        blk->instrs = new_instrs;
        blk->instr_cap = new_cap;
    }
    hb_ir_instr_t* instr = &blk->instrs[blk->instr_count++];
    memset(instr, 0, sizeof(hb_ir_instr_t));
    instr->op = op;
    /* The memo describes a fixed instruction list; appending changes that list, so drop it. This
     * is the ONLY mutation path (hb_ir_block_create is the only other writer), which is what makes
     * the memo safe to trust in the dispatcher. */
    blk->first_transfer_idx = HB_IR_TRANSFER_UNCOMPUTED;
    return instr;
}

#define EMIT_OP(name, op_code) \
    hb_ir_instr_t* hb_ir_emit_##name(hb_ir_builder_t* b, hb_ir_operand_t dst, hb_ir_operand_t src) { \
        hb_ir_instr_t* i = hb_ir_emit(b, op_code); \
        if (i) { i->dst = dst; i->src1 = src; } \
        return i; \
    }

EMIT_OP(mov, HB_IR_MOV)
EMIT_OP(lea, HB_IR_LEA)

hb_ir_instr_t* hb_ir_emit_binop(hb_ir_builder_t* b, hb_ir_op_t op, hb_ir_operand_t dst, hb_ir_operand_t a, hb_ir_operand_t b_op) {
    hb_ir_instr_t* i = hb_ir_emit(b, op);
    if (i) { i->dst = dst; i->src1 = a; i->src2 = b_op; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_unop(hb_ir_builder_t* b, hb_ir_op_t op, hb_ir_operand_t dst, hb_ir_operand_t src) {
    hb_ir_instr_t* i = hb_ir_emit(b, op);
    if (i) { i->dst = dst; i->src1 = src; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_load(hb_ir_builder_t* b, hb_ir_operand_t dst, hb_ir_operand_t addr) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_LOAD);
    if (i) { i->dst = dst; i->src1 = addr; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_store(hb_ir_builder_t* b, hb_ir_operand_t addr, hb_ir_operand_t src) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_STORE);
    if (i) { i->src1 = addr; i->src2 = src; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_fence(hb_ir_builder_t* b, hb_fence_kind_t kind) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_FENCE);
    if (i) i->src1 = hb_ir_imm((int64_t)kind, HB_SIZE_8);
    return i;
}

hb_ir_instr_t* hb_ir_emit_call(hb_ir_builder_t* b, uint64_t target) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_CALL);
    if (i) { i->target = target; i->src1 = hb_ir_none(); }
    return i;
}

hb_ir_instr_t* hb_ir_emit_ret(hb_ir_builder_t* b) {
    return hb_ir_emit(b, HB_IR_RET);
}

hb_ir_instr_t* hb_ir_emit_jmp(hb_ir_builder_t* b, uint64_t target) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_JMP);
    if (i) { i->target = target; i->src1 = hb_ir_none(); }
    return i;
}

hb_ir_instr_t* hb_ir_emit_jcc(hb_ir_builder_t* b, hb_cc_t cc, uint64_t target) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_Jcc);
    if (i) { i->cc = cc; i->target = target; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_push(hb_ir_builder_t* b, hb_ir_operand_t src) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_PUSH);
    if (i) { i->src1 = src; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_pop(hb_ir_builder_t* b, hb_ir_operand_t dst) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_POP);
    if (i) { i->dst = dst; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_cmp(hb_ir_builder_t* b, hb_ir_operand_t a, hb_ir_operand_t b_op) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_CMP);
    if (i) { i->src1 = a; i->src2 = b_op; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_test(hb_ir_builder_t* b, hb_ir_operand_t a, hb_ir_operand_t b_op) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_TEST);
    if (i) { i->src1 = a; i->src2 = b_op; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_setcc(hb_ir_builder_t* b, hb_cc_t cc, hb_ir_operand_t dst) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_SETcc);
    if (i) { i->cc = cc; i->dst = dst; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_cmovcc(hb_ir_builder_t* b, hb_cc_t cc, hb_ir_operand_t dst, hb_ir_operand_t src) {
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_CMOVcc);
    if (i) { i->cc = cc; i->dst = dst; i->src1 = src; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_host_call(hb_ir_builder_t* b, uint32_t thunk_id) {
    (void)thunk_id;
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_HOST_CALL);
    if (i) { /* thunk id stored in imm? placeholder */ }
    return i;
}

hb_ir_instr_t* hb_ir_emit_fault(hb_ir_builder_t* b, hb_result_t reason, const char* msg) {
    (void)reason;
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_FAULT);
    if (i) { i->comment = msg; }
    return i;
}

hb_ir_instr_t* hb_ir_emit_unsupported(hb_ir_builder_t* b, const char* feature, uint64_t guest_addr, uint8_t* bytes, size_t len) {
    (void)bytes;
    (void)len;
    hb_ir_instr_t* i = hb_ir_emit(b, HB_IR_UNSUPPORTED);
    if (i) {
        i->guest_addr = guest_addr;
        i->comment = feature;
        b->func->has_unsupported = true;
        free((void*)b->func->unsupported_reason);
        b->func->unsupported_reason = strdup(feature);
    }
    return i;
}

/* --- Operand helpers --- */
hb_ir_operand_t hb_ir_reg(hb_reg_t reg, hb_size_t size) {
    hb_ir_operand_t op = {0};
    op.type = HB_OP_REG;
    op.size = size;
    op.reg = reg;
    return op;
}

hb_ir_operand_t hb_ir_imm(int64_t val, hb_size_t size) {
    hb_ir_operand_t op = {0};
    op.type = HB_OP_IMM;
    op.size = size;
    op.imm = val;
    return op;
}

hb_ir_operand_t hb_ir_mem(hb_reg_t base, hb_reg_t index, uint8_t scale, int64_t disp, hb_size_t size) {
    return hb_ir_mem_segment(base, index, scale, disp, size, 0);
}

hb_ir_operand_t hb_ir_mem_segment(hb_reg_t base, hb_reg_t index, uint8_t scale,
                                  int64_t disp, hb_size_t size, uint8_t segment) {
    hb_ir_operand_t op = {0};
    op.type = HB_OP_MEM;
    op.size = size;
    op.mem.base = base;
    op.mem.index = index;
    op.mem.scale = scale;
    op.mem.disp = disp;
    op.mem.segment = segment;
    op.mem.addr32 = false;
    return op;
}

hb_ir_operand_t hb_ir_label(uint64_t id) {
    hb_ir_operand_t op = {0};
    op.type = HB_OP_LABEL;
    op.label = id;
    return op;
}

hb_ir_operand_t hb_ir_none(void) {
    hb_ir_operand_t op = {0};
    op.type = HB_OP_NONE;
    return op;
}

/* --- Register names --- */
static const char* reg_names[] = {
    "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
    "r8","r9","r10","r11","r12","r13","r14","r15",
    "rip",
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
    "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
    "xmm16","xmm17","xmm18","xmm19","xmm20","xmm21","xmm22","xmm23",
    "xmm24","xmm25","xmm26","xmm27","xmm28","xmm29","xmm30","xmm31"
};

const char* hb_reg_name(hb_reg_t reg) {
    if (reg < HB_REG_COUNT) return reg_names[reg];
    return "?";
}

static const char* reg_x86_names[] = {
    "eax","ecx","edx","ebx","esp","ebp","esi","edi",
    "eip",
    "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7"
};

const char* hb_reg_x86_name(hb_reg_x86_t reg) {
    if (reg < HB_REG_X86_COUNT) return reg_x86_names[reg];
    return "?";
}

/* --- Serialization --- */
char* hb_ir_func_to_json(const hb_ir_func_t* func) {
    if (!func) return strdup("{}");
    size_t cap = 4096;
    char* buf = malloc(cap);
    if (!buf) return NULL;
    int n = snprintf(buf, cap,
        "{\n  \"guest_addr\": \"0x%llx\",\n  \"guest_len\": %zu,\n  \"blocks\": %zu,\n  \"has_unsupported\": %s,\n  \"unsupported_reason\": \"%s\"\n}",
        (unsigned long long)func->guest_addr,
        func->guest_len,
        func->cfg ? func->cfg->block_count : 0,
        func->has_unsupported ? "true" : "false",
        func->unsupported_reason ? func->unsupported_reason : ""
    );
    if (n < 0 || (size_t)n >= cap) {
        free(buf);
        return strdup("{}");
    }
    return buf;
}

char* hb_ir_func_to_string(const hb_ir_func_t* func) {
    if (!func) return strdup("(null)");
    size_t cap = 4096;
    char* buf = malloc(cap);
    if (!buf) return NULL;
    int n = snprintf(buf, cap, "func @0x%llx (%zu bytes)\n",
        (unsigned long long)func->guest_addr, func->guest_len);
    if (func->cfg) {
        for (size_t i = 0; i < func->cfg->block_count; i++) {
            hb_ir_block_t* blk = func->cfg->blocks[i];
            n += snprintf(buf + n, cap - n, "  block %llu @0x%llx (%zu instrs)\n",
                (unsigned long long)blk->id,
                (unsigned long long)blk->guest_addr,
                blk->instr_count);
            if (n < 0 || (size_t)n >= cap) break;
        }
    }
    return buf;
}

hb_result_t hb_ir_func_validate(const hb_ir_func_t* func) {
    if (!func) return HB_ERR_INVALID_ARG;
    if (!func->cfg) return HB_ERR_INVALID_ARG;
    if (func->cfg->block_count == 0) return HB_ERR_INVALID_ARG;
    if (func->has_unsupported && !func->unsupported_reason) return HB_ERR_INVALID_ARG;
    return HB_OK;
}
