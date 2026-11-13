/* 仅通用 1.2.13 构建。本文件及 zlib 对象以 Z_PREFIX 编译，使 Pillow 1.3.1 与 CPython 1.2.13
 * 约定共存于一个 WASM。 */
/* Generic-only 1.2.13 build. Compile this file and its zlib objects with
 * Z_PREFIX so the Pillow1.3.1 and CPython1.2.13 contracts share one WASM. */
#include <stdint.h>
#include <string.h>
#include "zlib.h"
#define MAX_RGBA 16777216u
#define MAX_PNG (MAX_RGBA + 131072u)
extern uint8_t *png_input(void);
extern uint8_t *png_output(void);
const char *generic_zlib_version(void) { return zlibVersion(); }
static int fail(int code) {
    (void)code;
    return -1;
}
/* CASC 的通用 zlib 流遵循 CPython
 * zlib.compress(data,9)：method=DEFLATED、windowBits=15、memLevel=8、strategy=DEFAULT。块边界不刷新，只有末块使用
 * Z_FINISH；不支持字典/自定义模式。 */
/* Generic zlib stream for CASC: CPython zlib.compress(data,9) parameters are
 * method=DEFLATED, windowBits=15, memLevel=8, strategy=DEFAULT. Chunk boundaries
 * do not flush; only the final piece uses Z_FINISH. No dictionary/custom mode. */
static z_stream generic_stream;
static int generic_active;
int zlib_abort(void) {
    if (generic_active) {
        deflateEnd(&generic_stream);
        generic_active = 0;
    }
    return 0;
}
int zlib_begin(int level) {

    if (generic_active || level != 9)
        return fail(30);
    memset(&generic_stream, 0, sizeof(generic_stream));
    if (deflateInit2(&generic_stream, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return fail(31);
    generic_active = 1;
    return 0;
}
int zlib_push(uint32_t length, uint32_t finish) {
    if (!generic_active || length > MAX_RGBA || finish > 1) {
        zlib_abort();
        return fail(32);
    }
    generic_stream.next_in = png_input();
    generic_stream.avail_in = length;
    generic_stream.next_out = png_output();
    generic_stream.avail_out = MAX_PNG;
    int status = Z_OK;
    do {
        status = deflate(&generic_stream, finish ? Z_FINISH : Z_NO_FLUSH);
        if (status == Z_STREAM_END)
            break;
        if (status != Z_OK || !generic_stream.avail_out) {
            zlib_abort();
            return fail(33);
        }
    } while (generic_stream.avail_in || finish);
    int written = (int)(MAX_PNG - generic_stream.avail_out);
    if (finish) {
        if (status != Z_STREAM_END) {
            zlib_abort();
            return fail(34);
        }
        zlib_abort();
    }
    return written;
}
