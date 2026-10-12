#ifndef Q2_IMAGE_DECODE_H
#define Q2_IMAGE_DECODE_H
#include "lvgl.h"
#include "q2/result.h"
/* Owner-side decode outside Lua callbacks. Input <=256 KiB, dimensions <=512 each.
 * Output pixels use lv_malloc; caller owns them and releases with lv_free. */
q2_result_t q2_image_decode(const void *bytes, size_t size, lv_image_dsc_t *out);
#endif
