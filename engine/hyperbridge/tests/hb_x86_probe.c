/* HyperBridge i386 decode/lift probe for bulk ISA coverage.
 *
 * Mirrors hb_x64_probe.c but uses hb_decode_x86 / hb_lift_x86 so we can drive
 * 32-bit-only opcode families (PUSHA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS, ...)
 * against capstone's CS_MODE_32 view. Input: lines of hex bytes. Output: one
 * JSON object per line. */
#include "hb_decoder.h"
#include "hb_ir.h"
#include "hb_lifter.h"
#include "hb_result.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int main(void) {
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        uint8_t code[32];
        size_t len = parse_hex(line, code, sizeof(code));
        if (len == 0) {
            printf("{\"decode\":-1,\"len\":0,\"op\":\"BAD_HEX\",\"lift\":-1}\n");
            continue;
        }

        hb_decoded_t d;
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
        printf("{\"decode\":%d,\"len\":%u,\"op\":\"%s\",\"lift\":%d}\n",
               dr, (unsigned)dlen, op, lift);
    }
    return 0;
}
