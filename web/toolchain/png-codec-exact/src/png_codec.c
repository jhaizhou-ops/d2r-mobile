/* 窄范围 PNG8/RGBA 编解码器，行过滤/流处理直接移植自 Pillow 10.2.0 ZipEncode.c，见
 * reference/LICENSE 和 LICENSE-zlib。不使用
 * CPython/libpng；每个实例由单个线程拥有，固定缓冲区不可重入。 */
/* Narrow PNG8/RGBA codec. Row filtering/streaming are direct ports of Pillow
 * 10.2.0 ZipEncode.c. See reference/LICENSE and LICENSE-zlib. No CPython/libpng.
 * One worker owns each instance. Fixed buffers are non-reentrant. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#define MAX_PIXELS 4194304u
#define MAX_SIDE 8192u
#define MAX_RGBA (MAX_PIXELS * 4u)
#define MAX_PNG (MAX_RGBA + 131072u)
#define MAX_FILTERED (MAX_RGBA + MAX_SIDE)
#define MAX_ROW (MAX_SIDE * 4u + 1u)
#define BLOCK 65536u
static uint8_t input[MAX_PNG];
static uint8_t output[MAX_PNG];
static uint8_t filtered[MAX_FILTERED + 1u];
static uint8_t row_a[MAX_ROW], row_b[MAX_ROW];
static uint8_t prior[MAX_ROW], up[MAX_ROW], paeth_row[MAX_ROW], compressed[BLOCK];
static uint32_t width_result, height_result;
static int last_error;
uint8_t *png_input(void) { return input; }
uint8_t *png_output(void) { return output; }
uint32_t png_width(void) { return width_result; }
uint32_t png_height(void) { return height_result; }
int png_error(void) { return last_error; }
const char *png_zlib_version(void) { return zlibVersion(); }
static int fail(int code) {
    last_error = code;
    return -1;
}
static int geometry(uint32_t w, uint32_t h) {
    return w && h && w <= MAX_SIDE && h <= MAX_SIDE && (uint64_t)w * h <= MAX_PIXELS;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
static int chunk(uint32_t *used, const char type[4], const uint8_t *data, uint32_t len) {
    if (len > MAX_PNG - 12u || *used > MAX_PNG - len - 12u)
        return 0;
    uint8_t *at = output + *used;
    put32(at, len);
    memcpy(at + 4, type, 4);
    if (len)
        memcpy(at + 8, data, len);
    put32(at + 8 + len, (uint32_t)crc32(0, at + 4, len + 4));
    *used += len + 12;
    return 1;
}
static uint32_t distance(uint8_t v) { return v < 128 ? v : 256u - v; }
static uint8_t predictor(int a, int b, int c) {
    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
/* optimize=False 时过滤优先级为 NONE、UP、SUB、PAETH，只有严格改善才胜出；Pillow 此处刻意不考虑
 * AVERAGE。 */
/* optimize=False: filter precedence NONE, UP, SUB, PAETH; only strict
 * improvements win. Pillow deliberately does not consider AVERAGE here. */
static uint8_t *choose_filter(uint8_t *row, const uint8_t *previous, uint32_t bytes) {
    uint8_t *best = row;
    uint32_t sum = 0, score;
    row[0] = 0;
    up[0] = 2;
    prior[0] = 1;
    paeth_row[0] = 4;
    for (uint32_t i = 1; i <= bytes; i++)
        sum += distance(row[i]);
    if (sum) {
        score = 0;
        for (uint32_t i = 1; i <= bytes; i++) {
            up[i] = (uint8_t)(row[i] - previous[i]);
            score += distance(up[i]);
        }
        if (score < sum) {
            best = up;
            sum = score;
        }
    }
    if (sum) {
        score = 0;
        for (uint32_t i = 1; i <= bytes; i++) {
            prior[i] = (uint8_t)(row[i] - (i > 4 ? row[i - 4] : 0));
            score += distance(prior[i]);
        }
        if (score < sum) {
            best = prior;
            sum = score;
        }
    }
    if (sum) {
        score = 0;
        for (uint32_t i = 1; i <= bytes; i++) {
            uint8_t p = i > 4 ? predictor(row[i - 4], previous[i], previous[i - 4]) : previous[i];
            paeth_row[i] = (uint8_t)(row[i] - p);
            score += distance(paeth_row[i]);
        }
        if (score < sum)
            best = paeth_row;
    }
    return best;
}

/* 精确的生产保存约定：RGBA8，无元数据，默认 optimize=False、compress_level=-1、compress_type=-1，即
 * Z_FILTERED、window=15、memLevel=9。 */
/* Exact production save contract: RGBA8, no metadata, defaults optimize=False,
 * compress_level=-1, compress_type=-1 => Z_FILTERED, window=15, memLevel=9. */
int png_encode(uint32_t w, uint32_t h) {
    last_error = 0;
    width_result = height_result = 0;
    if (!geometry(w, h))
        return fail(1);
    if (strcmp(zlibVersion(), "1.3.1"))
        return fail(2);
    uint8_t ihdr[13] = {0};
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    memcpy(output, "\x89PNG\r\n\x1a\n", 8);
    uint32_t used = 8, y = 0, row_bytes = w * 4;
    if (!chunk(&used, "IHDR", ihdr, 13))
        return fail(3);
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 9, Z_FILTERED) != Z_OK)
        return fail(4);
    uint8_t *row = row_a, *previous = row_b;
    memset(previous, 0, row_bytes + 1);
    int done = 0, status = Z_OK;
    while (!done) {
        stream.next_out = compressed;
        stream.avail_out = BLOCK;
        if (stream.avail_in) {
            status = deflate(&stream, Z_NO_FLUSH);
            if (status < 0)
                break;
        }
        while (stream.avail_out && y < h) {
            memcpy(row + 1, input + y * row_bytes, row_bytes);
            stream.next_in = choose_filter(row, previous, row_bytes);
            stream.avail_in = row_bytes + 1;
            y++;
            status = deflate(&stream, Z_NO_FLUSH);
            if (status < 0)
                break;
            uint8_t *swap = row;
            row = previous;
            previous = swap;
        }
        if (status < 0)
            break;
        if (y == h) {
            while (stream.avail_out) {
                status = deflate(&stream, Z_FINISH);
                if (status == Z_STREAM_END) {
                    done = 1;
                    break;
                }
                if (status < 0)
                    break;
            }
        }
        if (status < 0)
            break;
        /* _idat.write 保留每个编码器结果，包括末尾零值。 */
        /* _idat.write preserves every encoder result, including a final zero. */
        if (!chunk(&used, "IDAT", compressed, BLOCK - stream.avail_out)) {
            status = Z_MEM_ERROR;
            break;
        }
    }
    deflateEnd(&stream);
    if (!done || status < 0)
        return fail(5);
    if (!chunk(&used, "IEND", NULL, 0))
        return fail(3);
    width_result = w;
    height_result = h;
    return (int)used;
}

/* 严格且经过认证的生产子集：非交错 RGBA8、有界 IDAT、有效 CRC、连续 IDAT
 * 段，无未知关键块或尾部载荷。忽略附属元数据，绝不将其用于色彩管理。 */
/* Strict authenticated production subset: noninterlaced RGBA8, bounded IDATs,
 * valid CRCs, contiguous IDAT run, no unknown critical chunks/trailing payload.
 * Ancillary metadata is ignored, never applied as color management. */
int png_decode(uint32_t length) {
    last_error = 0;
    width_result = height_result = 0;
    if (length < 45 || length > MAX_PNG || memcmp(input, "\x89PNG\r\n\x1a\n", 8))
        return fail(10);
    uint32_t pos = 8, w = 0, h = 0, expected = 0, chunks = 0;
    int saw_ihdr = 0, saw_idat = 0, ended_idat = 0, saw_iend = 0, stream_end = 0, initialized = 0,
        error = 0;
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    while (pos < length) {
        if (++chunks > 4096 || length - pos < 12) {
            error = 11;
            break;
        }
        uint32_t len = get32(input + pos);
        if (len > length - pos - 12) {
            error = 11;
            break;
        }
        const uint8_t *type = input + pos + 4, *data = input + pos + 8;
        for (unsigned i = 0; i < 4; i++)
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z')))
                error = 12;
        if (error || (type[2] & 32) || get32(data + len) != (uint32_t)crc32(0, type, len + 4)) {
            error = 12;
            break;
        }
        if (!memcmp(type, "IHDR", 4)) {
            if (saw_ihdr || pos != 8 || len != 13) {
                error = 13;
                break;
            }
            w = get32(data);
            h = get32(data + 4);
            if (!geometry(w, h) || memcmp(data + 8, "\x08\x06\0\0\0", 5)) {
                error = 14;
                break;
            }
            expected = (w * 4 + 1) * h;
            saw_ihdr = 1;
            if (inflateInit(&stream) != Z_OK) {
                error = 15;
                break;
            }
            initialized = 1;
            stream.next_out = filtered;
            stream.avail_out = expected + 1;
        } else if (!saw_ihdr) {
            error = 13;
            break;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (ended_idat || (stream_end && len)) {
                error = 16;
                break;
            }
            saw_idat = 1;
            stream.next_in = (Bytef *)data;
            stream.avail_in = len;
            while (stream.avail_in && !stream_end) {
                uInt before = stream.avail_in;
                int status = inflate(&stream, Z_NO_FLUSH);
                if (status == Z_STREAM_END)
                    stream_end = 1;
                else if (status != Z_OK || stream.total_out > expected ||
                         before == stream.avail_in) {
                    error = 17;
                    break;
                }
                if (!stream.avail_out) {
                    error = 17;
                    break;
                }
            }
            if (error || (stream_end && stream.avail_in)) {
                error = 17;
                break;
            }
        } else if (!memcmp(type, "IEND", 4)) {
            if (len || !saw_idat || !stream_end || stream.total_out != expected ||
                pos + 12 != length) {
                error = 18;
                break;
            }
            saw_iend = 1;
            pos += 12;
            break;
        } else {
            if (!(type[0] & 32)) {
                error = 19;
                break;
            }
            if (saw_idat)
                ended_idat = 1;
        }
        pos += len + 12;
    }
    if (initialized)
        inflateEnd(&stream);
    if (error)
        return fail(error);
    if (!saw_iend || pos != length)
        return fail(18);
    const uint32_t row_bytes = w * 4;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *in = filtered + y * (row_bytes + 1);
        uint8_t *row = output + y * row_bytes;
        const uint8_t *above = y ? row - row_bytes : NULL;
        uint8_t filter = *in++;
        if (filter > 4)
            return fail(20);
        for (uint32_t x = 0; x < row_bytes; x++) {
            int a = x >= 4 ? row[x - 4] : 0, b = above ? above[x] : 0,
                c = above && x >= 4 ? above[x - 4] : 0;
            int p = filter == 0   ? 0
                    : filter == 1 ? a
                    : filter == 2 ? b
                    : filter == 3 ? (a + b) / 2
                                  : predictor(a, b, c);
            row[x] = (uint8_t)(in[x] + p);
        }
    }
    width_result = w;
    height_result = h;
    return (int)(w * h * 4);
}
