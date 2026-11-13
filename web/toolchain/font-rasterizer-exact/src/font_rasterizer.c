/* Narrow Pillow 10.2.0 BASIC font driver, adapted from _imagingft.c and
 * Paste.c. Copyright (c) 1998-2007 Secret Labs AB; see reference/LICENSE.
 * Deliberately no CPython, RAQM, color fonts, files, or general Image API.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_GLYPH_H
#include FT_BITMAP_H
#include FT_STROKER_H

#define FONT_LIMIT (16u * 1024u * 1024u)
#define HEAP_LIMIT (16u * 1024u * 1024u)
#define PIXELS (4u * 1024u * 1024u)
#define TEXT_LIMIT 2048u
#define SIDE 8192
/* 必须保留 Pillow 的算术右移及中点向正方向取整规则。 */
/* Must retain Pillow's arithmetic right shift and ties-toward-positive rule. */
#define PIXEL(x) ((((x) + 32) & -64) >> 6)

enum {
    OK = 0,
    BAD_INPUT = 1,
    NO_FACE = 2,
    FREETYPE_ERROR = 3,
    BOUNDS = 4,
    BITMAP_MODE = 5,
    NO_MEMORY = 6
};
typedef struct {
    FT_UInt index;
    int advance;
} Glyph;
static unsigned char font_bytes[FONT_LIMIT], mask[PIXELS], canvas[PIXELS * 4];
static uint32_t text[TEXT_LIMIT];
/* 宽、高、X 偏移、Y 偏移、26.6 格式步进、上升及下降量。 */
/* width, height, offsetX, offsetY, advance26_6, ascent, descent */
static int32_t info[7];
static Glyph glyphs[TEXT_LIMIT];
static FT_Library library;
static FT_Face face;
static int last_error, ft_error;
static size_t heap_used;
typedef union {
    max_align_t align;
    size_t bytes;
} Allocation;
static void *bounded_alloc(FT_Memory unused, long size) {
    (void)unused;
    if (size <= 0 || (size_t)size > HEAP_LIMIT - heap_used)
        return NULL;
    Allocation *p = malloc(sizeof(*p) + (size_t)size);
    if (!p)
        return NULL;
    p->bytes = (size_t)size;
    heap_used += p->bytes;
    return p + 1;
}
static void bounded_free(FT_Memory unused, void *block) {
    (void)unused;
    if (block) {
        Allocation *p = (Allocation *)block - 1;
        heap_used -= p->bytes;
        free(p);
    }
}
static void *bounded_realloc(FT_Memory mem, long old_size, long size, void *block) {
    (void)old_size;
    if (!block)
        return bounded_alloc(mem, size);
    if (size <= 0) {
        bounded_free(mem, block);
        return NULL;
    }
    Allocation *p = (Allocation *)block - 1;
    size_t old = p->bytes;
    if ((size_t)size > HEAP_LIMIT - (heap_used - old))
        return NULL;
    Allocation *next = realloc(p, sizeof(*p) + (size_t)size);
    if (!next)
        return NULL;
    next->bytes = (size_t)size;
    heap_used = heap_used - old + next->bytes;
    return next + 1;
}
static struct FT_MemoryRec_ memory = {NULL, bounded_alloc, bounded_free, bounded_realloc};
static int fail(int code) {
    last_error = code;
    return code;
}
static int ft_fail(int code) {
    ft_error = code;
    return fail(FREETYPE_ERROR);
}
unsigned char *font_input(void) { return font_bytes; }
uint32_t *font_text(void) { return text; }
unsigned char *font_mask(void) { return mask; }
unsigned char *font_canvas(void) { return canvas; }
int32_t *font_info(void) { return info; }
int font_error(void) { return last_error; }
int font_ft_error(void) { return ft_error; }
int font_version(void) { return FREETYPE_MAJOR * 10000 + FREETYPE_MINOR * 100 + FREETYPE_PATCH; }
void font_close(void) {
    if (face) {
        FT_Done_Face(face);
        face = NULL;
    }
    if (library) {
        FT_Done_Library(library);
        library = NULL;
    }
    memset(font_bytes, 0, sizeof(font_bytes));
    memset(text, 0, sizeof(text));
    memset(info, 0, sizeof(info));
    last_error = ft_error = 0;
}
int font_open(uint32_t length, float size) {
    int error;
    FT_Size_RequestRec req;
    /* 写入不同字体前先关闭，字体对象借用 font_input。 */
    /* Close before writing a different font; its face borrows font_input. */
    if (face || library || !length || length > FONT_LIMIT || !isfinite(size) || size < 1 ||
        size > 256)
        return fail(BAD_INPUT);
    last_error = ft_error = 0;
    error = FT_New_Library(&memory, &library);
    if (error) {
        library = NULL;
        return ft_fail(error);
    }
    FT_Add_Default_Modules(library);
    error = FT_New_Memory_Face(library, font_bytes, length, 0, &face);
    if (!error) {
        req.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
        req.width = req.height = (FT_Long)(size * 64);
        req.horiResolution = req.vertResolution = 0;
        error = FT_Request_Size(face, &req);
    }
    if (error) {
        font_close();
        return ft_fail(error);
    }
    return OK;
}
/* 只支持生产锚点：默认 la，以及 ls、mb、mm。 */
/* Only production anchors: default la, ls, mb, mm. */
static int valid(uint32_t count, int anchor, int stroke) {
    if (!face)
        return fail(NO_FACE);
    if (count > TEXT_LIMIT || anchor < 0 || anchor > 3 || stroke < 0 || stroke > 2)
        return fail(BAD_INPUT);
    for (uint32_t i = 0; i < count; i++)
        if (text[i] > 0x10ffff || (text[i] >= 0xd800 && text[i] <= 0xdfff) || text[i] == 10 ||
            text[i] == 13)
            return fail(BAD_INPUT);
    return OK;
}
static int layout(uint32_t count) {
    FT_UInt previous = 0;
    int64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        glyphs[i].index = FT_Get_Char_Index(face, text[i]);
        int error = FT_Load_Glyph(face, glyphs[i].index, FT_LOAD_DEFAULT);
        if (error)
            return ft_fail(error);
        FT_Pos advance = face->glyph->metrics.horiAdvance;
        if (advance < -(1 << 20) || advance > (1 << 20))
            return fail(BOUNDS);
        glyphs[i].advance = (int)advance;
        if (FT_HAS_KERNING(face) && previous && glyphs[i].index) {
            FT_Vector delta;
            if (!FT_Get_Kerning(face, previous, glyphs[i].index, ft_kerning_default, &delta)) {
                if (delta.x < -(1 << 20) || delta.x > (1 << 20))
                    return fail(BOUNDS);
                /* 刻意保留 Pillow BASIC 的特殊行为：这里不是 delta.x。 */
                /* Intentional Pillow BASIC quirk: this is NOT delta.x. */
                int kern = (int)PIXEL(delta.x);
                glyphs[i - 1].advance += kern;
                total += kern;
            }
        }
        total += advance;
        if (total < -(1 << 28) || total > (1 << 28))
            return fail(BOUNDS);
        previous = glyphs[i].index;
    }
    info[4] = (int32_t)total;
    info[5] = (int32_t)PIXEL(face->size->metrics.ascender);
    info[6] = -(int32_t)PIXEL(face->size->metrics.descender);
    return OK;
}
static int bbox(uint32_t count, int anchor, int flags) {
    int position = 0, xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (uint32_t i = 0; i < count; i++) {
        int px = PIXEL(position);
        position += glyphs[i].advance;
        int advanced = PIXEL(position);
        if (advanced > xmax)
            xmax = advanced;
        int error = FT_Load_Glyph(face, glyphs[i].index, flags);
        if (error)
            return ft_fail(error);
        FT_Glyph glyph = NULL;
        error = FT_Get_Glyph(face->glyph, &glyph);
        if (error)
            return ft_fail(error);
        FT_BBox box;
        FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &box);
        FT_Done_Glyph(glyph);
        if (box.xMin < -SIDE || box.xMax > SIDE || box.yMin < -SIDE || box.yMax > SIDE)
            return fail(BOUNDS);
        if (box.xMax + px > xmax)
            xmax = (int)box.xMax + px;
        if (box.xMin + px < xmin)
            xmin = (int)box.xMin + px;
        if (box.yMax > ymax)
            ymax = (int)box.yMax;
        if (box.yMin < ymin)
            ymin = (int)box.yMin;
    }
    int xa = 0, ya = 0;
    if (count) {
        if (anchor == 2 || anchor == 3)
            xa = PIXEL(position / 2);
        if (anchor == 0)
            ya = (int)PIXEL(face->size->metrics.ascender);
        else if (anchor == 2)
            ya = ymin;
        else if (anchor == 3)
            ya = (int)PIXEL((face->size->metrics.ascender + face->size->metrics.descender) / 2);
    }
    info[0] = xmax - xmin;
    info[1] = ymax - ymin;
    info[2] = xmin - xa;
    info[3] = ya - ymax;
    return OK;
}
static int bounded_image(int width, int height) {
    return width >= 0 && height >= 0 && width <= SIDE && height <= SIDE &&
           (uint64_t)width * height <= PIXELS;
}
int font_measure(uint32_t count, int anchor, int stroke) {
    last_error = ft_error = 0;
    memset(info, 0, sizeof(info));
    if (valid(count, anchor, stroke) || layout(count) || bbox(count, anchor, FT_LOAD_DEFAULT))
        return last_error;
    info[0] += stroke * 2;
    info[1] += stroke * 2;
    info[2] -= stroke;
    info[3] -= stroke;
    return bounded_image(info[0], info[1]) ? OK : fail(BOUNDS);
}
static unsigned char div255(unsigned int value) {
    value += 128;
    return (unsigned char)(((value >> 8) + value) >> 8);
}
int font_render(uint32_t count, int anchor, int stroke, float start_x, float start_y) {
    FT_Stroker stroker = NULL;
    FT_Glyph glyph = NULL;
    FT_Bitmap converted;
    int converted_ready = 0, error = 0;
    last_error = ft_error = 0;
    memset(info, 0, sizeof(info));
    if (!isfinite(start_x) || !isfinite(start_y) || fabsf(start_x) >= 1 || fabsf(start_y) >= 1)
        return fail(BAD_INPUT);
    if (valid(count, anchor, stroke) || layout(count))
        return last_error;
    int flags = stroke ? FT_LOAD_NO_BITMAP : FT_LOAD_DEFAULT;
    if (bbox(count, anchor, flags))
        return last_error;
    info[0] += stroke * 2 + (int)ceilf(start_x);
    info[1] += stroke * 2 + (int)ceilf(start_y);
    info[2] -= stroke;
    info[3] -= stroke;
    int width = info[0], height = info[1];
    if (!bounded_image(width, height))
        return fail(BOUNDS);
    memset(mask, 0, (size_t)width * height);
    if (!count || !width || !height)
        return OK;
    if (stroke) {
        error = FT_Stroker_New(library, &stroker);
        if (error)
            goto done;
        FT_Stroker_Set(stroker, stroke * 64, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND,
                       0);
    }
    int x = 0, xmin = 0, ymax = 0;
    for (uint32_t i = 0; i < count; i++) {
        int px = PIXEL(x);
        error = FT_Load_Glyph(face, glyphs[i].index, flags | FT_LOAD_RENDER);
        if (error)
            goto done;
        if (face->glyph->bitmap_top > ymax)
            ymax = face->glyph->bitmap_top;
        if (face->glyph->bitmap_left + px < xmin)
            xmin = face->glyph->bitmap_left + px;
        x += glyphs[i].advance;
    }
    x = (int)((-xmin + stroke + start_x) * 64);
    int y = (int)((-ymax - stroke - start_y) * 64);
    if (!stroker)
        flags |= FT_LOAD_RENDER;
    for (uint32_t i = 0; i < count; i++) {
        int px = PIXEL(x), py = PIXEL(y), xx, yy;
        error = FT_Load_Glyph(face, glyphs[i].index, flags);
        if (error)
            goto done;
        FT_Bitmap bitmap;
        if (stroker) {
            error = FT_Get_Glyph(face->glyph, &glyph);
            if (error)
                goto done;
            error = FT_Glyph_Stroke(&glyph, stroker, 1);
            if (error)
                goto done;
            FT_Vector origin = {0, 0};
            error = FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, &origin, 1);
            if (error)
                goto done;
            FT_BitmapGlyph bg = (FT_BitmapGlyph)glyph;
            bitmap = bg->bitmap;
            xx = px + bg->left;
            yy = -(py + bg->top);
        } else {
            bitmap = face->glyph->bitmap;
            xx = px + face->glyph->bitmap_left;
            yy = -(py + face->glyph->bitmap_top);
        }
        if ((!bitmap.buffer && bitmap.rows) || bitmap.width > SIDE || bitmap.rows > SIDE) {
            fail(BOUNDS);
            goto done;
        }
        unsigned int scale = 1;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO || bitmap.pixel_mode == FT_PIXEL_MODE_GRAY2 ||
            bitmap.pixel_mode == FT_PIXEL_MODE_GRAY4) {
            scale = bitmap.pixel_mode == FT_PIXEL_MODE_MONO    ? 255
                    : bitmap.pixel_mode == FT_PIXEL_MODE_GRAY2 ? 85
                                                               : 17;
            if (!converted_ready) {
                FT_Bitmap_Init(&converted);
                converted_ready = 1;
            }
            error = FT_Bitmap_Convert(library, &bitmap, &converted, 1);
            if (error)
                goto done;
            bitmap = converted;
        }
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
            fail(BITMAP_MODE);
            goto done;
        }
        int x0 = xx < 0 ? -xx : 0;
        int x1 = (int)bitmap.width;
        if (xx + x1 > width)
            x1 = width - xx;
        const unsigned char *source = bitmap.buffer;
        for (unsigned int by = 0; by < bitmap.rows; by++, yy++) {
            if (yy >= 0 && yy < height) {
                for (int k = x0; k < x1; k++) {
                    unsigned int a = source[k] * scale;
                    unsigned char *target = mask + (size_t)yy * width + xx + k;
                    if (a)
                        *target = (unsigned char)(a + div255(*target * (255 - a)));
                }
            }
            source += bitmap.pitch;
        }
        x += glyphs[i].advance;
        if (glyph) {
            FT_Done_Glyph(glyph);
            glyph = NULL;
        }
    }
done:
    if (glyph)
        FT_Done_Glyph(glyph);
    if (converted_ready)
        FT_Bitmap_Done(library, &converted);
    if (stroker)
        FT_Stroker_Done(stroker);
    if (error)
        return ft_fail(error);
    return last_error;
}
static void draw_mask(int width, int height, int channels, int dx, int dy, uint32_t ink) {
    int mw = info[0], mh = info[1];
    for (int sy = 0; sy < mh; sy++) {
        int yy = dy + sy;
        if (yy < 0 || yy >= height)
            continue;
        for (int sx = 0; sx < mw; sx++) {
            int xx = dx + sx;
            if (xx < 0 || xx >= width)
                continue;
            unsigned int a = mask[(size_t)sy * mw + sx];
            if (!a)
                continue;
            unsigned char *out = canvas + ((size_t)yy * width + xx) * channels;
            for (int c = 0; c < channels; c++) {
                /* Pillow Paste.c 的 fill_mask_L：透明 RGB 接收完整墨色。 */
                /* Pillow Paste.c fill_mask_L: transparent RGB receives full ink. */
                unsigned int channel_mask = channels == 4 && c != 3 && out[3] == 0 ? 255 : a;
                unsigned int v = (ink >> (8 * c)) & 255;
                out[c] = div255(out[c] * (255 - channel_mask) + v * channel_mask);
            }
        }
    }
}
/* 画布由调用方初始化；浮点起点遵循 ImageDraw 的 modf 转 C float。stroke
 * 非零时，即使墨色相同，也分别绘制描边遮罩及无描边前景，ImageDraw 不合并两种遮罩。 */
/* Canvas is caller-initialized. Float starts match ImageDraw's modf -> C float.
 * stroke != 0 always draws both stroke mask and unstroked foreground, even if
 * their inks are identical. ImageDraw does NOT merge those two masks. */
int font_draw(uint32_t count, int anchor, int stroke, double x, double y, int width, int height,
              int channels, uint32_t ink, uint32_t stroke_ink) {
    if (!isfinite(x) || !isfinite(y) || fabs(x) > SIDE * 2 || fabs(y) > SIDE * 2 ||
        !bounded_image(width, height) || (channels != 1 && channels != 4))
        return fail(BAD_INPUT);
    double integral_x, integral_y;
    float sx = (float)modf(x, &integral_x), sy = (float)modf(y, &integral_y);
    if (stroke) {
        if (font_render(count, anchor, stroke, sx, sy))
            return last_error;
        draw_mask(width, height, channels, (int)integral_x + info[2], (int)integral_y + info[3],
                  stroke_ink);
    }
    if (font_render(count, anchor, 0, sx, sy))
        return last_error;
    draw_mask(width, height, channels, (int)integral_x + info[2], (int)integral_y + info[3], ink);
    return OK;
}
