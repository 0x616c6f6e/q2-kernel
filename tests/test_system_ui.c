#include "q2/lvgl.h"
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static q2_result_t at(q2_kernel_t *k, uint64_t time) {
    ticks = time;
    return q2_kernel_poll(k, 16);
}
static uint32_t color(unsigned x, unsigned y) {
    return frame[y * 320 + x] & 0xffffff;
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_pixel_backend_t backend = {1, sizeof(backend), NULL, 320, 240, flush};
    q2_kernel_t *k = NULL;
    q2_lua_runtime_t *runtime = NULL;
    q2_lvgl_t *display = NULL;
    const q2_component_t *component;
    q2_system_ui_config_t config = {true, true, 32, 30000, 10000};
    q2_system_ui_status_t status = {true, 12, 34, -1, false, false, false};
    q2_system_ui_state_t state;
    bool consumed;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_lvgl_create(k, &backend, &display) == Q2_OK);
    CHECK(q2_lvgl_activity(display, &consumed) == Q2_ERR_STATE);
    config.status_bar_height = 200;
    CHECK(q2_lvgl_system_configure(display, &config) == Q2_ERR_ARGUMENT);
    config.status_bar_height = 32;
    CHECK(q2_lvgl_system_configure(display, &config) == Q2_OK);
    status.battery_percent = 101;
    CHECK(q2_lvgl_system_status(display, &status) == Q2_ERR_ARGUMENT);
    status.battery_percent = -1;
    CHECK(q2_lvgl_system_status(display, &status) == Q2_OK);
    CHECK(q2_lvgl_component(display, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_lvgl_system_configure(display, &config) == Q2_ERR_STATE);
    CHECK(q2_lua_create(k, &runtime) == Q2_OK);
    /* The APP viewport excludes chrome, but physical pointer coordinates include it. */
    CHECK(run(runtime, "local q=require('q2');local n=0;return {on_start=function() "
                       "assert(q.ui.resolution()=='320x240'); assert(q.ui.width()==320 and q.ui.height()==208);"
                       "assert(q.ui.clear());assert(q.ui.background(0x123456));"
                       "assert(q.ui.button(20,20,'Test'));end,on_event=function(e) "
                       "assert(e.type=='ui.click');n=n+1;assert(n==1);"
                       "assert(q.ui.background(0x345678)) end}") == Q2_OK);
    CHECK(at(k, 32) == Q2_OK);
    CHECK(color(0, 0) == 0x0b1018 && color(319, 239) == 0x123456);
    q2_app_status_t before, after;
    CHECK(q2_lua_status(runtime, &before) == Q2_OK && before.running);
    CHECK(at(k, 29999) == Q2_OK);
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && !state.screensaver_active);
    CHECK(at(k, 30000) == Q2_OK);
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && state.screensaver_active);
    CHECK(state.content_top == 32 && state.content_height == 208);
    CHECK(at(k, 30032) == Q2_OK && color(0, 0) == 0 && color(319, 239) == 0);
    uint32_t saved[320 * 240];
    memcpy(saved, frame, sizeof(frame));
    CHECK(at(k, 40032) == Q2_OK);
    CHECK(memcmp(saved, frame, sizeof(frame)) != 0); /* Clock actually changes position. */
    unsigned lit = 0;
    for (unsigned i = 0; i < 320 * 240; i++)
        lit += (frame[i] & 0xffffff) != 0;
    CHECK(lit > 100 && lit < 4000);
    CHECK(q2_lvgl_pointer(display, 40, 64, false) == Q2_OK); /* Hover does not wake. */
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && state.screensaver_active);
    CHECK(q2_lvgl_pointer(display, 40, 64, true) == Q2_OK);
    CHECK(q2_lvgl_pointer(display, 40, 64, false) == Q2_OK);
    CHECK(at(k, 40064) == Q2_OK);
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && !state.screensaver_active);
    CHECK(color(319, 239) == 0x123456); /* Wake must not click through. */
    CHECK(q2_lua_status(runtime, &after) == Q2_OK && after.running && after.instance == before.instance);
    CHECK(q2_lvgl_pointer(display, 40, 64, true) == Q2_OK);
    CHECK(q2_lvgl_pointer(display, 40, 64, false) == Q2_OK);
    CHECK(at(k, 40096) == Q2_OK && at(k, 40128) == Q2_OK);
    CHECK(color(319, 239) == 0x345678 && color(0, 0) == 0x0b1018);
    /* A held pointer postpones idle even across a full timeout. */
    CHECK(q2_lvgl_pointer(display, 300, 220, true) == Q2_OK);
    CHECK(at(k, 80000) == Q2_OK);
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && !state.screensaver_active);
    CHECK(q2_lvgl_pointer(display, 300, 220, false) == Q2_OK);
    CHECK(at(k, 110032) == Q2_OK);
    CHECK(q2_lvgl_activity(display, &consumed) == Q2_OK && consumed);
    CHECK(q2_lvgl_activity(display, &consumed) == Q2_OK && !consumed);
    CHECK(at(k, 110064) == Q2_OK);
    CHECK(q2_lua_stop(runtime) == Q2_OK);
    CHECK(color(0, 0) == 0x0b1018 && color(319, 239) == 0x111827);
    /* APP teardown/relaunch does not erase chrome or reset the system idle deadline. */
    CHECK(run(runtime, "local q=require('q2');assert(q.ui.clear());return {}") == Q2_OK);
    CHECK(at(k, 140096) == Q2_OK);
    CHECK(q2_lvgl_system_state(display, &state) == Q2_OK && state.screensaver_active);
    for (unsigned step = 1; step <= 20; step++) {
        CHECK(at(k, 140096 + step * 10000) == Q2_OK);
        unsigned pixels = 0;
        for (unsigned y = 0; y < 240; y++)
            for (unsigned x = 0; x < 320; x++)
                if (color(x, y)) {
                    CHECK(x >= 8 && x < 312 && y >= 8 && y < 232);
                    pixels++;
                }
        CHECK(pixels == lit); /* No clipping after repeated absolute clock moves. */
    }
    CHECK(q2_lua_destroy(&runtime) == Q2_OK);
    CHECK(at(k, 340128) == Q2_OK && color(0, 0) == 0);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_lvgl_destroy(&display) == Q2_OK);
    /* Short/wide displays use the small clock and never underflow the movement range. */
    backend.height = 64;
    config.status_bar = false;
    config.idle_timeout_ms = 100;
    config.clock_move_interval_ms = 100;
    ticks = 0;
    memset(frame, 0, sizeof(frame));
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_lvgl_create(k, &backend, &display) == Q2_OK);
    CHECK(q2_lvgl_system_configure(display, &config) == Q2_OK);
    CHECK(q2_lvgl_system_status(display, &status) == Q2_OK);
    CHECK(q2_lvgl_component(display, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(at(k, 100) == Q2_OK);
    for (unsigned step = 1; step <= 10; step++) {
        CHECK(at(k, 100 + step * 100) == Q2_OK);
        unsigned pixels = 0;
        for (unsigned y = 0; y < 64; y++)
            for (unsigned x = 0; x < 320; x++)
                if (color(x, y)) {
                    CHECK(x >= 8 && x < 312 && y >= 8 && y < 56);
                    pixels++;
                }
        CHECK(pixels > 100);
    }
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_lvgl_destroy(&display) == Q2_OK);
    printf("PASS: %u system UI checks; viewport, idle, clock motion, wake filtering and APP lifetime\n",
           checks);
    return 0;
}
