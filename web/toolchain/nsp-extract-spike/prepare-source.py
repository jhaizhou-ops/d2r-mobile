#!/usr/bin/env python3
"""针对固定上游版本执行小范围、失败即关闭的转换。 | Small fail-closed transformations against one pinned upstream revision."""

from pathlib import Path
import sys

root = Path(sys.argv[1])
p = root / "extkeys.c"
s = p.read_text()


def replace_once(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit("Pinned upstream source mismatch")
    s = s.replace(old, new)


replace_once(
    "static int get_kv(FILE *f, char **key, char **value) {",
    """#define NSP_KEY_LINE_BYTES 4096
static char nsp_parser_line[NSP_KEY_LINE_BYTES + 3]; /* 内容 + CRLF + NUL | content + CRLF + NUL */
void nsp_clear_parser(void) { nsp_zero(nsp_parser_line, sizeof nsp_parser_line); }
static int get_kv(FILE *f, char **key, char **value) {""",
)
replace_once(
    "    static char line[1024];",
    "    char *line = nsp_parser_line;\n    nsp_clear_parser();",
)
replace_once("fgets(line, (int)sizeof(line), f)", "fgets(line, (int)sizeof(nsp_parser_line), f)")
replace_once(
    "    if (errno != 0)\n        return -2;",
    """    if (errno != 0)
        return -2;

    /* 每次解析器调用保留一条物理行。RSA 大小的转储条目可能不被 hactool 识别，
     * 但绝不能被拆成第二条密钥记录。与有界 JS 校验器保持一致，为 CRLF 和 NUL 预留空间。
     * Keep one physical line per parser call. RSA-sized dump entries may be
     * unrecognized by hactool, but must never split into a second key record.
     * Match the bounded JS validator, reserving room for CRLF and the NUL. */
    size_t line_length = strlen(line), content_length = line_length;
    if (content_length && line[content_length - 1] == '\\n')
        content_length--;
    if (content_length && line[content_length - 1] == '\\r')
        content_length--;
    if (content_length > NSP_KEY_LINE_BYTES) {
        if (!line_length || line[line_length - 1] != '\\n') {
            int next;
            do { next = fgetc(f); } while (next != '\\n' && next != EOF);
        }
        nsp_clear_parser();
        return ferror(f) ? -2 : -1;
    }""",
)
replace_once(
    "realloc(settings->known_titlekeys.titlekeys, 2 * (settings->known_titlekeys.count + 1) * sizeof(titlekey_entry_t))",
    "nsp_resize_keys(settings->known_titlekeys.titlekeys, settings->known_titlekeys.count * sizeof(titlekey_entry_t), 2 * (settings->known_titlekeys.count + 1) * sizeof(titlekey_entry_t))",
)
replace_once(
    "                settings_add_titlekey(settings, rights_id, titlekey);",
    "                settings_add_titlekey(settings, rights_id, titlekey);\n                nsp_zero(titlekey, sizeof titlekey);",
)
p.write_text(s)
# 上游将其 64 位名称映射到 fseek；wasm32 上的 long 为 32 位，故使用 off_t。
# Upstream maps its 64-bit name to fseek; long is 32-bit on wasm32. Use off_t.
p = root / "utils.h"
s = p.read_text()
old = "#define fseeko64 fseek"
if s.count(old) != 1:
    raise SystemExit("Pinned upstream seek shim mismatch")
p.write_text(s.replace(old, "#define fseeko64 fseeko"))
# 边界只支持 NCA2/3；必须在上游进入旧版密钥或文件路径之前拒绝 NCA0。
# The boundary supports only NCA2/3. Reject legacy formats before upstream
# touches legacy key indexes or the CLI-only base file handle.
p = root / "nca.c"
s = p.read_text()
replace_once(
    "    aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0x400, 0, 0x200);",
    """    aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0x400, 0, 0x200);
    if (dec_header.magic != MAGIC_NCA3 && dec_header.magic != MAGIC_NCA2) {
        free_aes_ctx(hdr_aes_ctx);
        return 0;
    }""",
)
# 首次解密仅写入前 0x400 字节；检查输入中的分区是否为空，不能读取未初始化的输出。
# The first decrypt writes only 0x400 bytes. Detect absent sections in the
# initialized input, not the not-yet-decrypted output section headers.
replace_once(
    "if (dec_header.fs_headers[i]._0x148[0] != 0 || memcmp(dec_header.fs_headers[i]._0x148, dec_header.fs_headers[i]._0x148 + 1, 0xB7))",
    "if (ctx->header.fs_headers[i]._0x148[0] != 0 || memcmp(ctx->header.fs_headers[i]._0x148, ctx->header.fs_headers[i]._0x148 + 1, 0xB7))",
)
p.write_text(s)
