#include "image_decode.h"
#include "src/libs/lodepng/lodepng.h"
#include "src/libs/tjpgd/tjpgd.h"
#include <stddef.h>
#include <string.h>

typedef struct {
    const unsigned char *bytes;
    size_t size, offset;
    unsigned char *pixels;
} jpeg_stream_t;

static size_t jpeg_read(JDEC *decoder, uint8_t *out, size_t count) {
    jpeg_stream_t *s = decoder->device;
    if (count > s->size - s->offset)
        count = s->size - s->offset;
    if (out)
        memcpy(out, s->bytes + s->offset, count);
    s->offset += count;
    return count;
}
static int jpeg_write(JDEC *decoder, void *rgb, JRECT *rect) {
    jpeg_stream_t *s = decoder->device;
    if (rect->right >= decoder->width || rect->bottom >= decoder->height)
        return 0;
    const unsigned char *p = rgb;
    for (unsigned y = rect->top; y <= rect->bottom; ++y)
        for (unsigned x = rect->left; x <= rect->right; ++x, p += 3) {
            unsigned char *pixel = s->pixels + (y * decoder->width + x) * 4;
            /* LVGL TJPGD emits BGR888 in this pinned version. */
            pixel[0] = p[0];
            pixel[1] = p[1];
            pixel[2] = p[2];
            pixel[3] = 255;
        }
    return 1;
}
static bool valid_size(unsigned w, unsigned h) {
    return w && h && w <= 512 && h <= 512;
}
q2_result_t q2_image_decode(const void *bytes, size_t size, lv_image_dsc_t *out) {
    if (!bytes || !out || size < 8 || size > 262144)
        return Q2_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    unsigned w = 0, h = 0;
    unsigned char *pixels = NULL;
    const unsigned char *data = bytes;
    if (!memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        LodePNGState state;
        lodepng_state_init(&state);
        unsigned error = lodepng_inspect(&w, &h, &state, bytes, size);
        lodepng_state_cleanup(&state);
        if (error)
            return Q2_ERR_FORMAT;
        if (!valid_size(w, h))
            return Q2_ERR_LIMIT;
        /* LVGL's pinned LodePNG returns a draw buffer, not upstream's byte array.
         * Copy its pixels into our common PNG/JPEG ownership format before destroying it. */
        lv_draw_buf_t *decoded = NULL;
        error = lodepng_decode32((unsigned char **)&decoded, &w, &h, bytes, size);
        if (error) {
            if (decoded)
                lv_draw_buf_destroy(decoded);
            return Q2_ERR_FORMAT;
        }
        pixels = lv_malloc((size_t)w * h * 4);
        if (!pixels) {
            lv_draw_buf_destroy(decoded);
            return Q2_ERR_MEMORY;
        }
        for (unsigned y = 0; y < h; ++y)
            memcpy(pixels + y * w * 4, decoded->data + y * decoded->header.stride, w * 4);
        lv_draw_buf_destroy(decoded);
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
            unsigned char red = pixels[i];
            pixels[i] = pixels[i + 2];
            pixels[i + 2] = red;
        }
    } else if (data[0] == 0xff && data[1] == 0xd8) {
        _Alignas(max_align_t) unsigned char scratch[8192];
        JDEC decoder;
        jpeg_stream_t stream = {.bytes = bytes, .size = size};
        JRESULT error = jd_prepare(&decoder, jpeg_read, scratch, sizeof(scratch), &stream);
        if (error != JDR_OK)
            return error == JDR_FMT3 || error == JDR_FMT2 ? Q2_ERR_UNSUPPORTED : Q2_ERR_FORMAT;
        w = decoder.width;
        h = decoder.height;
        if (!valid_size(w, h))
            return Q2_ERR_LIMIT;
        pixels = lv_malloc((size_t)w * h * 4);
        if (!pixels)
            return Q2_ERR_MEMORY;
        stream.pixels = pixels;
        if (jd_decomp(&decoder, jpeg_write, 0) != JDR_OK) {
            lv_free(pixels);
            return Q2_ERR_FORMAT;
        }
    } else {
        return Q2_ERR_UNSUPPORTED;
    }
    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.cf = LV_COLOR_FORMAT_ARGB8888;
    out->header.w = w;
    out->header.h = h;
    out->header.stride = w * 4;
    out->data = pixels;
    out->data_size = w * h * 4;
    return Q2_OK;
}
