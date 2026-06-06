#include "hb_decoder.h"
#include "hb_ir.h"
#include "hb_lifter.h"
#include "hb_result.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char* s, uint8_t* out, size_t cap) {
    size_t n = 0;
    int high = -1;
    while (*s && *s != '\n' && *s != '\r') {
        if (isspace((unsigned char)*s)) {
            s++;
            continue;
        }
        int v = hex_nibble((unsigned char)*s++);
        if (v < 0) return 0;
        if (high < 0) {
            high = v;
        } else {
            if (n >= cap) return 0;
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    return high < 0 ? n : 0;
}

static void print_op(int idx, bool present, bool is_reg, bool is_mem, bool is_imm, int reg, uint8_t size, int64_t imm) {
    printf(",\"op%d_present\":%s", idx, present ? "true" : "false");
    printf(",\"op%d_is_reg\":%s", idx, is_reg ? "true" : "false");
    printf(",\"op%d_is_mem\":%s", idx, is_mem ? "true" : "false");
    printf(",\"op%d_is_imm\":%s", idx, is_imm ? "true" : "false");
    printf(",\"op%d_reg\":%d", idx, reg);
    printf(",\"op%d_size\":%u", idx, (unsigned)size);
    printf(",\"op%d_imm\":%lld", idx, (long long)imm);
}

int main(void) {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        uint8_t code[32];
        size_t len = parse_hex(line, code, sizeof(code));
        if (len == 0) {
            printf("{\"decode\":-1,\"len\":0,\"op\":\"BAD_HEX\",\"lift\":-1}\n");
            continue;
        }

        hb_decoded_t d;
        memset(&d, 0, sizeof(d));
        hb_result_t dr = hb_decode_x86(code, len, 0x100000ULL, &d);
        int lift = 999;
        const char* op = "NONE";
        uint8_t dlen = 0;
        if (dr == HB_OK) {
            op = hb_opcode_name(d.opcode);
            dlen = d.len;
            hb_ir_func_t* func = hb_ir_func_create(0x100000ULL, d.len);
            hb_ir_block_t* block = func ? hb_ir_block_create(0, 0x100000ULL) : NULL;
            hb_ir_builder_t* builder = (func && block) ? hb_ir_builder_create(func) : NULL;
            if (func && block && builder) {
                hb_ir_cfg_add_block(func->cfg, block);
                func->cfg->entry = block;
                hb_ir_builder_set_block(builder, block);
                lift = hb_lift_x86(&d, builder);
            } else {
                lift = HB_ERR_OUT_OF_MEMORY;
            }
            if (builder) hb_ir_builder_destroy(builder);
            if (func) hb_ir_func_destroy(func);
        }

        // Print base info
        printf("{\"decode\":%d,\"len\":%u,\"op\":\"%s\",\"lift\":%d", dr, (unsigned)dlen, op, lift);

        // Print operands info
        print_op(1, d.op1.present, d.op1.is_reg, d.op1.is_mem, d.op1.is_imm, d.op1.reg, d.op1.size, d.op1.imm);
        print_op(2, d.op2.present, d.op2.is_reg, d.op2.is_mem, d.op2.is_imm, d.op2.reg, d.op2.size, d.op2.imm);
        print_op(3, d.op3.present, d.op3.is_reg, d.op3.is_mem, d.op3.is_imm, d.op3.reg, d.op3.size, d.op3.imm);

        // Print memory reference info if present in any operand
        bool has_mem = d.op1.is_mem || d.op2.is_mem || d.op3.is_mem;
        int mem_base = -1;
        int mem_index = -1;
        int mem_scale = 0;
        long long mem_disp = 0;
        int mem_segment = 0;
        bool mem_addr32 = false;

        if (d.op1.is_mem) {
            mem_base = d.op1.mem.base;
            mem_index = d.op1.mem.index;
            mem_scale = d.op1.mem.scale;
            mem_disp = (long long)d.op1.mem.disp;
            mem_segment = d.op1.mem.segment;
            mem_addr32 = d.op1.mem.addr32;
        } else if (d.op2.is_mem) {
            mem_base = d.op2.mem.base;
            mem_index = d.op2.mem.index;
            mem_scale = d.op2.mem.scale;
            mem_disp = (long long)d.op2.mem.disp;
            mem_segment = d.op2.mem.segment;
            mem_addr32 = d.op2.mem.addr32;
        } else if (d.op3.is_mem) {
            mem_base = d.op3.mem.base;
            mem_index = d.op3.mem.index;
            mem_scale = d.op3.mem.scale;
            mem_disp = (long long)d.op3.mem.disp;
            mem_segment = d.op3.mem.segment;
            mem_addr32 = d.op3.mem.addr32;
        }

        printf(",\"has_mem\":%s", has_mem ? "true" : "false");
        printf(",\"mem_base\":%d", mem_base);
        printf(",\"mem_index\":%d", mem_index);
        printf(",\"mem_scale\":%d", mem_scale);
        printf(",\"mem_disp\":%lld", mem_disp);
        printf(",\"mem_segment\":%d", mem_segment);
        printf(",\"mem_addr32\":%s", mem_addr32 ? "true" : "false");

        // Print ModRM info
        printf(",\"has_modrm\":%s", d.has_modrm ? "true" : "false");
        printf(",\"mod\":%u", d.mod);
        printf(",\"reg_op\":%u", d.reg_op);
        printf(",\"rm\":%u", d.rm);

        printf("}\n");
    }
    return 0;
}
