// 窄范围真实 Capstone 5 ARM64 绑定，不重写指令解码器。
// Narrow, real Capstone 5 ARM64 binding. No instruction decoder is reimplemented.
#include <capstone/capstone.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
static csh handle;
static char output[16384];
static size_t used;
static int failed;
static void add(const char *fmt, ...) {
    if (failed)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(output + used, sizeof(output) - used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(output) - used) {
        failed = 1;
        return;
    }
    used += (size_t)n;
}
static void quoted(const char *s) {
    add("\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '"')
            add("\\%c", c);
        else if (c < 32)
            add("\\u%04x", c);
        else
            add("%c", c);
    }
    add("\"");
}
static void reglist(cs_regs regs, uint8_t count) {
    add("[");
    for (unsigned i = 0; i < count; i++) {
        if (i)
            add(",");
        quoted(cs_reg_name(handle, regs[i]));
    }
    add("]");
}
const char *decode_word(uint32_t word, uint32_t address) {
    if (!handle) {
        if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK)
            return "{\"error\":\"cs_open\"}";
        if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
            return "{\"error\":\"cs_option\"}";
    }
    uint8_t bytes[4] = {word, word >> 8, word >> 16, word >> 24};
    cs_insn *ins;
    size_t count = cs_disasm(handle, bytes, 4, address, 1, &ins);
    if (!count)
        return "null";
    used = 0;
    failed = 0;
    cs_regs reads, writes;
    uint8_t nr = 0, nw = 0;
    cs_err error = cs_regs_access(handle, ins, reads, &nr, writes, &nw);
    add("{\"address\":%u,\"size\":%u,\"mnemonic\":", address, ins->size);
    quoted(ins->mnemonic);
    add(",\"op_str\":");
    quoted(ins->op_str);
    add(",\"regs_error\":%u,\"reads\":", (unsigned)error);
    reglist(reads, nr);
    add(",\"writes\":");
    reglist(writes, nw);
    add(",\"operands\":[");
    cs_arm64 *detail = &ins->detail->arm64;
    for (unsigned i = 0; i < detail->op_count; i++) {
        cs_arm64_op *op = &detail->operands[i];
        if (i)
            add(",");
        add("{\"reg\":");
        quoted(op->type == ARM64_OP_REG ? cs_reg_name(handle, op->reg) : "");
        // 使用十进制字符串跨越 JS 边界，保留有符号 64 位立即数。
        // Decimal string preserves signed 64-bit immediates across the JS boundary.
        add(",\"imm\":\"%lld\"}", op->type == ARM64_OP_IMM ? (long long)op->imm : 0LL);
    }
    add("]}");
    cs_free(ins, count);
    return failed ? "{\"error\":\"binding output overflow\"}" : output;
}
