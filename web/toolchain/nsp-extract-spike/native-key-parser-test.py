#!/usr/bin/env python3
"""只用合成数据测试补丁后的固定 hactool 密钥解析器。用法：python3 native-key-parser-test.py /path/to/unmodified/pinned/hactool。需要本地 C 编译器，不下载源码，不读取任何密钥文件。
Exercise the patched pinned hactool key parser with synthetic data only.

Usage: python3 native-key-parser-test.py /path/to/unmodified/pinned/hactool
Requires a local C compiler. Does not download sources or read any key files.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="d2r-synthetic-key-parser-") as directory:
    work = Path(directory)
    for name in ["extkeys.c", "utils.h", "nca.c"]:
        shutil.copyfile(source / name, work / name)
    subprocess.run([sys.executable, str(here / "prepare-source.py"), str(work)], check=True)
    (work / "fixture.c").write_text(r"""
#include <assert.h>
#include "extkeys.c"
void nsp_zero(void *ptr, size_t size) { memset(ptr, 0, size); }
static FILE *input(void) { FILE *f = tmpfile(); assert(f); return f; }
static void repeat(FILE *f, size_t count) { while (count--) fputc('a', f); }
static void next_key(FILE *f) {
    fputs("header_key = ", f); repeat(f, 64); fputc('\n', f);
}
static void check_header(FILE *f) {
    char *key, *value;
    assert(get_kv(f, &key, &value) == 0);
    assert(strcmp(key, "header_key") == 0);
    assert(strlen(value) == 64);
}
int main(void) {
    char *key, *value;
    FILE *f = input();
    fputs("eticket_rsa_keypair = ", f); repeat(f, 1056); fputs("\r\n", f);
    next_key(f); rewind(f);
    assert(get_kv(f, &key, &value) == 0);
    assert(strcmp(key, "eticket_rsa_keypair") == 0);
    assert(strlen(value) == 1056);
    check_header(f); fclose(f);
    /* Exact content limit: CRLF/LF are not part of the 4096-byte budget. */
    const char *endings[] = {"", "\n", "\r\n"};
    for (unsigned i = 0; i < 3; i++) {
        f = input(); fputs("extra = ", f); repeat(f, 4088); fputs(endings[i], f); rewind(f);
        assert(get_kv(f, &key, &value) == 0);
        assert(strcmp(key, "extra") == 0 && strlen(value) == 4088);
        assert(get_kv(f, &key, &value) == 1);
        fclose(f);
    }
    /* Reject the whole oversized physical line; do not parse its remainder. */
    f = input(); fputs("extra = ", f); repeat(f, 9000);
    fputs("fake_key = aa\n", f); next_key(f); rewind(f);
    assert(get_kv(f, &key, &value) == -1 && key == NULL && value == NULL);
    for (size_t i = 0; i < sizeof nsp_parser_line; i++) assert(nsp_parser_line[i] == 0);
    check_header(f); fclose(f);
    nsp_clear_parser();
    puts("PASS synthetic native key-line size and non-fragmentation checks");
    return 0;
}
""")
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-O2",
            "-ffunction-sections",
            "-fdata-sections",
            "-include",
            str(here / "quiet.h"),
            "-I" + str(source),
            "-I" + str(source / "mbedtls/include"),
            str(work / "fixture.c"),
            "-Wl,--gc-sections",
            "-o",
            str(work / "fixture"),
        ],
        check=True,
    )
    subprocess.run([str(work / "fixture")], check=True)
