#include "lvgl.h"
#include "q2/lvgl.h"
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Generated 2x2 solid RGBA PNG: exercises the real pinned decoder. */
static const unsigned char test_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
    0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x10,
    0x5a, 0x6d, 0xf2, 0x1f, 0x84, 0x19, 0x60, 0x0c, 0x00, 0x41, 0x32, 0x07, 0xc1, 0x5a, 0x67,
    0xe9, 0x89, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
static unsigned checks, flushes;
static uint64_t ticks;
static bool fail_flush;
static uint32_t frame[320 * 240];
#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr);                                               \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void *allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}
static void release(void *ctx, void *p) {
    (void)ctx;
    free(p);
}
static uint64_t now(void *ctx) {
    (void)ctx;
    return ticks;
}
static q2_result_t flush(void *ctx, unsigned x, unsigned y, unsigned w, unsigned h, const uint32_t *pixels,
                         size_t stride) {
    (void)ctx;
    if (fail_flush)
        return Q2_ERR_IO;
    CHECK(x + w <= 320 && y + h <= 240 && stride >= w * 4);
    for (unsigned row = 0; row < h; row++)
        memcpy(frame + (y + row) * 320 + x, (const unsigned char *)pixels + row * stride, w * 4);
    flushes++;
    return Q2_OK;
}
static const char manifest[] =
    "{\"format\":2,\"id\":\"2d7f3e18-b368-43e7-9698-eb29015fc1f8\",\"name\":\"LVGL Test\","
    "\"version\":\"1.0.0\",\"entry\":\"main.lua\",\"type\":\"app\",\"api\":2,"
    "\"capabilities\":{\"required\":[\"display.ui\"],\"optional\":[\"display.text\"]},"
    "\"resources\":{\"lua_heap_kib\":64}}";
static const char *grants[] = {"display.ui", "display.text"};
static q2_result_t run(q2_lua_runtime_t *r, const char *code) {
    q2_result_t e = q2_lua_start(r, manifest, strlen(manifest), code, strlen(code), grants, 2);
    if (e != Q2_OK) {
        q2_app_status_t status;
        q2_lua_status(r, &status);
        fprintf(stderr, "Lua error: %s\n", status.error);
    }
    return e;
}
static q2_result_t advance(q2_kernel_t *k) {
    ticks += 32;
    return q2_kernel_poll(k, 16);
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_pixel_backend_t backend = {1, sizeof(backend), NULL, 320, 240, flush};
    q2_kernel_t *k = NULL;
    q2_lua_runtime_t *runtime = NULL;
    q2_lvgl_t *display = NULL;
    const q2_component_t *component;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_lvgl_create(k, &backend, &display) == Q2_OK);
    unsigned physical_width = 0, physical_height = 0;
    CHECK(q2_lvgl_resolution(display, &physical_width, &physical_height) == Q2_OK);
    CHECK(physical_width == 320 && physical_height == 240);
    CHECK(q2_lvgl_resolution(NULL, &physical_width, &physical_height) == Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_resolution(display, NULL, &physical_height) == Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_resolution(display, &physical_width, NULL) == Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_pointer(display, 10, 10, true) == Q2_ERR_STATE);
    const char *owner = "2d7f3e18-b368-43e7-9698-eb29015fc1f8";
    CHECK(q2_lvgl_register_png(display, owner, "assets/test.png", test_png, sizeof(test_png)) == Q2_OK);
    CHECK(q2_lvgl_register_png(display, owner, "assets/test.png", test_png, sizeof(test_png)) ==
          Q2_ERR_EXISTS);
    CHECK(q2_lvgl_register_png(display, owner, "assets/../bad.png", test_png, sizeof(test_png)) ==
          Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_register_png(display, owner, "assets/bad.png", "bad", 3) == Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_register_png(display, "71f147c6-3604-4474-9e3d-af2d949fb38f", "assets/foreign.png",
                               test_png, sizeof(test_png)) == Q2_OK);

    CHECK(q2_lvgl_component(display, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    /* Resolve real glyphs, not the fallback placeholder square. Existing Latin
     * metrics stay on
     * Montserrat while Chinese is resolved by the embedded font. */
    const lv_font_t *text_font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    lv_font_glyph_dsc_t glyph;
    CHECK(lv_font_get_glyph_dsc(text_font, &glyph, 'A', 0) && glyph.resolved_font == text_font);
#if Q2_ENABLE_CJK_FONT
    /* Simplified/traditional titles, Cantonese, punctuation and a non-BMP surname glyph. */
    const uint32_t chinese[] = {0x4e2d, 0x6587, 0x97f3, 0x4e50, 0x6a02, 0x9ad4, 0x9f98,
                                0x5605, 0x55ba, 0x5497, 0x5572, 0x5187, 0x5514, 0x20bb7, 0xff0c};
    CHECK(text_font->fallback != NULL);
    for (unsigned i = 0; i < sizeof(chinese) / sizeof(chinese[0]); ++i) {
        CHECK(lv_font_get_glyph_dsc(text_font, &glyph, chinese[i], 0));
        CHECK(!glyph.is_placeholder && glyph.resolved_font == text_font->fallback);
        CHECK(glyph.box_w > 0 && glyph.box_h > 0);
    }
#else
    CHECK(text_font->fallback == NULL);
    CHECK(!lv_font_get_glyph_dsc(text_font, &glyph, 0x4e2d, 0) || glyph.is_placeholder);
    CHECK(lv_font_get_glyph_dsc(text_font, &glyph, '0', 0) && !glyph.is_placeholder);
    CHECK(lv_font_get_glyph_dsc(text_font, &glyph, 0xf04b, 0) && !glyph.is_placeholder);
#endif
    CHECK(q2_lvgl_destroy(&display) == Q2_ERR_BUSY);
    CHECK(q2_lua_create(k, &runtime) == Q2_OK);
    const char *code = "local q=require('q2');local label,button;return {on_start=function() "
                       "assert(q.ui.background(0x123456));label=assert(q.ui.label(20,10,'Counter: 0'));"
                       "button=assert(q.ui.button(20,64,'Click'));end,on_event=function(e) "
                       "assert(e.type=='ui.click' and e.value==button and e.text=='Click');"
                       "assert(q.ui.set_text(label,'Clicked'));assert(q.ui.background(0x345678))end}";
    CHECK(q2_lua_start(runtime, manifest, strlen(manifest), code, strlen(code), NULL, 0) ==
          Q2_ERR_PERMISSION);
    CHECK(run(runtime, code) == Q2_OK);
    CHECK(advance(k) == Q2_OK && flushes > 0);
    CHECK((frame[0] & 0xffffff) == 0x123456);
    unsigned other = 0;
    for (unsigned i = 0; i < 320 * 240; i++)
        if ((frame[i] & 0xffffff) != 0x123456)
            other++;
    CHECK(other > 1000); /* Real rendered glyphs and button, not just a clear-color stub. */
    CHECK(q2_lvgl_pointer(display, -1, 5, true) == Q2_ERR_ARGUMENT);
    CHECK(q2_lvgl_pointer(display, 40, 80, true) == Q2_OK);
    CHECK(q2_lvgl_pointer(display, 40, 80, false) == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[0] & 0xffffff) == 0x345678);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(run(runtime,
              "local q=require('q2'); assert(q.ui.resolution()=='320x240'); assert(q.ui.width()==320 and q.ui.height()==240);"
              "assert(q.ui.wallpaper('assets/test.png')); local h=assert(q.ui.image(5,5,'assets/test.png'));"
              "local v,e=q.ui.set_text(h,'not a label'); assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.image(0,0,'assets/foreign.png'); assert(v==nil and e.code=='not_found');"
              "v,e=q.ui.wallpaper('assets/../bad.png'); assert(v==nil and e.code=='not_found');"
              "assert(q.ui.button(240,100,'Small',80)); return {}") == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[0] & 0xffffff) == 0x12ab34);
    CHECK((frame[239 * 320 + 319] & 0xffffff) == 0x12ab34);
    CHECK(q2_lvgl_register_png(display, owner, "assets/late.png", test_png, sizeof(test_png)) ==
          Q2_ERR_STATE);
    CHECK(q2_lvgl_clear_images(display) == Q2_ERR_BUSY);
    /* Faulted flush must release the LVGL draw buffer and preserve cleanup ownership for retry. */
    fail_flush = true;
    CHECK(q2_lua_stop(runtime) == Q2_ERR_IO);
    q2_app_status_t state;
    CHECK(q2_lua_status(runtime, &state) == Q2_OK && !state.running && state.instance != 0);
    fail_flush = false;
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK((frame[0] & 0xffffff) == 0x111827);
    CHECK(q2_lvgl_clear_images(display) == Q2_OK);

    /* A platform can accumulate edges while synchronous flush blocks its UI owner.
     * Replaying several press/release pairs before poll must emit one click per pair. */
    CHECK(run(runtime,
              "local q=require('q2');local n=0;local id=assert(q.ui.button(20,20,'Rapid'));"
              "return {on_event=function(e) assert(e.type=='ui.click' and e.value==id);"
              "n=n+1;assert(q.ui.background(0x120000+n)) end}") == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    for (unsigned batch = 1; batch <= 4; batch++) {
        for (unsigned tap = 0; tap < 8; tap++) {
            CHECK(q2_lvgl_pointer(display, 60, 40, true) == Q2_OK);
            CHECK(q2_lvgl_pointer(display, 60, 40, false) == Q2_OK);
        }
        CHECK(advance(k) == Q2_OK);
        CHECK(advance(k) == Q2_OK);
        CHECK((frame[0] & 0xffffff) == 0x120000 + batch * 8);
    }
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(q2_lvgl_register_png(display, owner, "assets/replaced.png", test_png, sizeof(test_png)) == Q2_OK);
    CHECK(run(runtime, "local q=require('q2');assert(q.ui.wallpaper('assets/replaced.png'));return {}") ==
          Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[0] & 0xffffff) == 0x12ab34);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(run(runtime,
              "local q=require('q2');local icon;return {on_start=function() "
              "icon=assert(q.ui.image(40,40,'assets/replaced.png',40));"
              "assert(q.ui.label(0,100,'X','center'));"
              "local v,e=q.ui.label(0,0,'bad','unknown');assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.image(300,0,'assets/replaced.png',40);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.image(0,0,'assets/replaced.png',0);assert(v==nil and e.code=='invalid_argument');"
              "end,on_event=function(e) assert(e.value==icon and "
              "e.text=='');assert(q.ui.background(0x345678)) end}") == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[45 * 320 + 45] & 0xffffff) == 0x12ab34);
    CHECK((frame[39 * 320 + 39] & 0xffffff) == 0x111827);
    unsigned glyphs = 0;
    for (unsigned y = 100; y < 128; y++)
        for (unsigned x = 0; x < 320; x++)
            if ((frame[y * 320 + x] & 0xffffff) != 0x111827) {
                CHECK(x >= 140 && x <= 180); /* Actual glyph pixels, not guessed string width. */
                glyphs++;
            }
    CHECK(glyphs > 10);
    CHECK(q2_lvgl_pointer(display, 60, 60, true) == Q2_OK);
    CHECK(q2_lvgl_pointer(display, 60, 60, false) == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[0] & 0xffffff) == 0x345678);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(q2_lvgl_clear_images(display) == Q2_OK);

    CHECK(run(runtime,
              "local q=require('q2');return {on_start=function() "
              "local old=assert(q.ui.label(0,0,'old'));assert(q.ui.clear());"
              "local fresh=assert(q.ui.label(0,0,'new'));assert(fresh>old);"
              "local v,e=q.ui.set_text(old,'bad');assert(v==nil and e.code=='not_found');"
              "v,e=q.ui.button(319,239,'outside');assert(v==nil and e.code=='invalid_argument');"
              "assert(q.ui.clear());for i=1,48 do assert(q.ui.label(0,0,'quota')) end;"
              "v,e=q.ui.label(0,0,'extra');assert(v==nil and e.code=='quota_exceeded');"
              "assert(q.ui.clear());for i=1,70 do assert(q.display.text(0,0,'reuse')) end end}") == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(run(runtime,
              "local q=require('q2');local id=assert(q.ui.qrcode(20,20,'Q2-Kernel',160));"
              "local v,e=q.ui.qrcode(0,0,'',120);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.qrcode(280,0,'x',80);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.qrcode(0,0,'x',57);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.qrcode(0,0,'x',9223372036854775807);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.set_text(id,'change');assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.size(id,160,100);assert(v==nil and e.code=='invalid_argument');"
              "v,e=q.ui.position(id,200,0);assert(v==nil and e.code=='invalid_argument');"
              "assert(q.ui.position(id,20,20));return {}") == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK((frame[20 * 320 + 20] & 0xffffff) == 0xffffff);
    CHECK((frame[47 * 320 + 47] & 0xffffff) == 0x000000);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(
        run(runtime,
            "local q=require('q2');for i=1,60 do local h=assert(q.ui.qrcode(0,0,'reuse',120));"
            "assert(q.ui.clear());local v,e=q.ui.position(h,0,0);assert(v==nil and e.code=='not_found') end;"
            "return {}") == Q2_OK);
    CHECK(q2_lua_destroy(&runtime) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_lvgl_destroy(&display) == Q2_OK);
    /* A second complete lifecycle proves global LVGL ownership is released. */
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_lvgl_create(k, &backend, &display) == Q2_OK);
    CHECK(q2_lvgl_component(display, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(advance(k) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_lvgl_destroy(&display) == Q2_OK);
    printf("PASS: %u LVGL checks; rendering, click-to-Lua, permissions, quotas and cleanup\n", checks);
    return 0;
}
