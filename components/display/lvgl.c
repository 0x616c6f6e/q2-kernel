#include "q2/lvgl.h"
#include "image_decode.h"
#include "lvgl.h"
#include "q2/log.h"
#include "q2/network.h"
/* Pinned LVGL 9.3 exposes decoder session storage in this internal header only. */
#include "src/draw/lv_image_decoder_private.h"
#if LV_USE_QRCODE
#include "src/libs/qrcode/qrcodegen.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if Q2_ENABLE_CJK_FONT
/* Generated from the pinned OFL font asset, embedded once for both UI sizes. */
extern const unsigned char q2_chinese_ttf[];
extern const size_t q2_chinese_ttf_size;
#endif

typedef struct {
    lv_obj_t *object, *label;
    uint32_t handle, remote;
    int x, y;
    bool text_slot, qrcode;
} widget_t;
typedef struct {
    char app_id[37], name[96];
    lv_image_dsc_t png, image;
    lv_image_decoder_dsc_t decoder;
} image_t;
#define REMOTE_MAX 4u
typedef struct {
    q2_caller_t caller;
    uint32_t id;
    uint64_t request, deadline;
    char url[Q2_TEXT_MAX];
    lv_image_dsc_t image;
    q2_result_t result;
    const q2_network_binary_t *transport;
} remote_t;
#define SCENE_MAX (Q2_RUNTIME_MAX + 2u)
typedef struct {
    uint64_t instance;
    char app_id[37];
    unsigned layer, canvas_height;
    lv_obj_t *root, *wallpaper;
    widget_t widgets[Q2_UI_OBJECT_MAX];
} scene_t;
struct q2_lvgl {
    q2_kernel_t *kernel;
    q2_pixel_backend_t backend;
    q2_component_t component;
    lv_display_t *display;
    lv_indev_t *input;
    uint32_t *buffer;
    lv_font_t text_20, text_40;
#if Q2_ENABLE_CJK_FONT
    lv_font_t *cjk_20, *cjk_40;
#endif
    scene_t scenes[SCENE_MAX], *scene;
    image_t images[Q2_UI_IMAGE_MAX];
    remote_t remote[REMOTE_MAX];
    unsigned image_count, image_pixels;
    lv_obj_t *bar, *bar_clock, *bar_status, *saver, *saver_clock;
    q2_system_ui_config_t system_config;
    q2_system_ui_status_t system_status;
    uint64_t last_activity, last_move;
    unsigned move_step;
    bool sleeping, pointer_suppressed;
    uint64_t last_tick, foreground, lifecycle_instance;
    unsigned lifecycle_layer;
    char lifecycle_id[37], factory_ids[Q2_RUNTIME_MAX][37];
    unsigned factory_count, revision;
    bool locked, wake_pending, persistent, preferences_loaded;
    q2_lifecycle_config_t preferences;
    q2_caller_t settings_caller;
    uint32_t next_handle;
    int pointer_x, pointer_y;
    bool pressed, started, full_refresh;
    q2_result_t pending_error;
};
/* LVGL owns process-global state; reject a second provider rather than sharing it accidentally. */
static q2_lvgl_t *active;

static bool remote_used(q2_lvgl_t *d, uint32_t id) {
    for (unsigned i = 0; i < SCENE_MAX; ++i)
        for (unsigned j = 0; j < Q2_UI_OBJECT_MAX; ++j)
            if (d->scenes[i].widgets[j].object && d->scenes[i].widgets[j].remote == id)
                return true;
    return false;
}
static q2_result_t remote_clear(remote_t *r) {
    if (r->request) {
        q2_result_t e = r->transport->close(r->transport->context, &r->caller, r->request);
        if (e != Q2_OK && e != Q2_ERR_NOT_FOUND)
            return e;
    }
    if (r->image.data) {
        lv_image_cache_drop(&r->image);
        lv_free((void *)r->image.data);
    }
    memset(r, 0, sizeof(*r));
    return Q2_OK;
}
static q2_result_t remote_release(q2_lvgl_t *d, uint64_t instance) {
    for (unsigned i = 0; i < REMOTE_MAX; ++i)
        if (d->remote[i].id && (!instance || d->remote[i].caller.instance == instance)) {
            q2_result_t e = remote_clear(&d->remote[i]);
            if (e != Q2_OK)
                return e;
        }
    return Q2_OK;
}
static void remote_poll(q2_lvgl_t *d) {
    for (unsigned i = 0; i < REMOTE_MAX; ++i) {
        remote_t *r = &d->remote[i];
        if (!r->id || (!r->request && r->result != Q2_ERR_BUSY))
            continue;
        if (!r->request) {
            uint64_t now = 0;
            q2_kernel_time(d->kernel, &now);
            q2_result_t begin = Q2_ERR_TIMEOUT;
            if (now + 100 < r->deadline)
                begin = r->transport->get(r->transport->context, &r->caller, r->url,
                                          (unsigned)(r->deadline - now), &r->request);
            if (begin == Q2_ERR_BUSY || begin == Q2_ERR_LIMIT)
                continue; // Detached workers may still occupy transport slots after rapid skips.
            if (begin != Q2_OK) {
                r->result = begin;
                const char *name = "unknown_error";
                q2_result_name(begin, &name);
                Q2_LOGW(d->kernel, "display", "remote image fallback: %s", name);
                continue;
            }
        }
        q2_network_response_t response;
        q2_result_t e = r->transport->read(r->transport->context, &r->caller, r->request, &response);
        if (e == Q2_ERR_BUSY)
            continue;
        if (r->result == Q2_ERR_BUSY) {
            if (e == Q2_OK && (response.code < 200 || response.code >= 300))
                e = Q2_ERR_IO;
            if (e == Q2_OK)
                e = q2_image_decode(response.body, response.size, &r->image);
            r->result = e;
        }
        e = r->transport->close(r->transport->context, &r->caller, r->request);
        if (e != Q2_OK && e != Q2_ERR_NOT_FOUND)
            return; // Retain the handle until the backend accepts cancellation.
        r->request = 0;
        if (r->result == Q2_OK) {
            for (unsigned scene = 0; scene < SCENE_MAX; ++scene)
                for (unsigned j = 0; j < Q2_UI_OBJECT_MAX; ++j) {
                    widget_t *w = &d->scenes[scene].widgets[j];
                    if (w->object && w->remote == r->id)
                        lv_image_set_src(w->object, &r->image);
                }
            Q2_LOGI(d->kernel, "display", "remote image ready %ux%u", r->image.header.w, r->image.header.h);
        } else {
            const char *name = "unknown_error";
            q2_result_name(r->result, &name);
            Q2_LOGW(d->kernel, "display", "remote image fallback: %s", name);
        }
        return; // Decode at most one bounded image per owner tick, never inside Lua.
    }
}
static bool fonts_create(q2_lvgl_t *d) {
    d->text_20 = lv_font_montserrat_20;
    d->text_40 = lv_font_montserrat_40;
    d->text_20.fallback = d->text_40.fallback = NULL;
#if Q2_ENABLE_CJK_FONT
    /* Small caches rasterize on demand; both sizes share one read-only font. */
    d->cjk_20 =
        lv_tiny_ttf_create_data_ex(q2_chinese_ttf, q2_chinese_ttf_size, 20, LV_FONT_KERNING_NONE, 128);
    d->cjk_40 = lv_tiny_ttf_create_data_ex(q2_chinese_ttf, q2_chinese_ttf_size, 40, LV_FONT_KERNING_NONE, 64);
    if (!d->cjk_20 || !d->cjk_40)
        return false;
    d->text_20.fallback = d->cjk_20;
    d->text_40.fallback = d->cjk_40;
#endif
    return true;
}

static void graphics_destroy(q2_lvgl_t *d) {
    /* Delete every text object before releasing its font/cache, including failed boot. */
    if (d->display)
        lv_display_delete(d->display);
#if Q2_ENABLE_CJK_FONT
    if (d->cjk_20)
        lv_tiny_ttf_destroy(d->cjk_20);
    if (d->cjk_40)
        lv_tiny_ttf_destroy(d->cjk_40);
    d->cjk_20 = d->cjk_40 = NULL;
#endif
    lv_deinit();
    d->display = NULL;
    d->input = NULL;
}

static unsigned content_top(const q2_lvgl_t *d) {
    return d->system_config.status_bar ? d->system_config.status_bar_height : 0;
}
static unsigned content_height(const q2_lvgl_t *d) {
    return d->backend.height - content_top(d);
}
static unsigned view_height(const q2_lvgl_t *d) {
    return d->scene && d->scene->layer == 1 ? d->system_config.status_bar_height : content_height(d);
}
static unsigned draw_height(const q2_lvgl_t *d) {
    return d->scene->canvas_height ? d->scene->canvas_height : view_height(d);
}
static q2_result_t preferences_load(q2_lvgl_t *d);
static q2_result_t registered_unlock_key(q2_lvgl_t *d, const char *name);
static lv_obj_t *panel(lv_obj_t *parent, unsigned width, unsigned height, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj) {
        lv_obj_remove_style_all(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(obj, (int)width, (int)height);
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    return obj;
}
static void status_labels(q2_lvgl_t *d) {
    char clock[8], indicators[80];
    const q2_system_ui_status_t *s = &d->system_status;
    if (s->clock_valid)
        snprintf(clock, sizeof(clock), "%02u:%02u", s->hour, s->minute);
    else
        strcpy(clock, "--:--");
    char battery[20];
    if (s->battery_percent < 0)
        strcpy(battery, "BAT --");
    else
        snprintf(battery, sizeof(battery), "%s%d%%", s->charging ? LV_SYMBOL_CHARGE : "", s->battery_percent);
    snprintf(indicators, sizeof(indicators), "%s%s%s", s->wifi_connected ? LV_SYMBOL_WIFI "  " : "",
             s->bluetooth_connected ? LV_SYMBOL_BLUETOOTH "  " : "", battery);
    if (d->bar_clock) {
        lv_label_set_text(d->bar_clock, clock);
        lv_label_set_text(d->bar_status, indicators);
    }
    if (d->saver_clock)
        lv_label_set_text(d->saver_clock, clock);
}
static void idle_update(q2_lvgl_t *d, uint64_t now) {
    if (*d->lifecycle_id || !d->system_config.screensaver)
        return;
    /* A held touch is activity, not an idle screen. Web key sources likewise refresh while held. */
    if (d->pressed || d->pointer_suppressed)
        d->last_activity = now;
    if (!d->sleeping && now - d->last_activity >= d->system_config.idle_timeout_ms) {
        d->sleeping = true;
        d->last_move = now;
        d->move_step = 0;
        lv_obj_remove_flag(d->saver, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(d->input, NULL);
    }
    if (d->sleeping && now - d->last_move >= d->system_config.clock_move_interval_ms) {
        d->last_move = now;
        d->move_step++;
        int width = (d->backend.width >= 200 && d->backend.height >= 80) ? 160 : (int)d->backend.width - 16;
        int height = (d->backend.width >= 200 && d->backend.height >= 80) ? 50 : 28;
        unsigned dx = d->backend.width - (unsigned)width - 16;
        unsigned dy = d->backend.height - (unsigned)height - 16;
        /* Initial centering sets an alignment anchor; movement uses absolute screen coordinates. */
        lv_obj_set_align(d->saver_clock, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(d->saver_clock, 8 + (int)((d->move_step * 37u) % (dx + 1)),
                       8 + (int)((d->move_step * 23u) % (dy + 1)));
    }
}
static q2_result_t system_create(q2_lvgl_t *d, lv_obj_t *screen) {
    d->scene->root = panel(screen, d->backend.width, content_height(d), 0x111827);
    if (!d->scene->root)
        return Q2_ERR_MEMORY;
    lv_obj_set_y(d->scene->root, (int)content_top(d));
    if (d->system_config.status_bar && !*d->lifecycle_id) {
        d->bar = panel(screen, d->backend.width, content_top(d), 0x0b1018);
        if (!d->bar)
            return Q2_ERR_MEMORY;
        d->bar_clock = lv_label_create(d->bar);
        d->bar_status = lv_label_create(d->bar);
        if (!d->bar_clock || !d->bar_status)
            return Q2_ERR_MEMORY;
        lv_obj_set_width(d->bar_clock, (int)d->backend.width - 16);
        lv_label_set_long_mode(d->bar_clock, LV_LABEL_LONG_CLIP);
        lv_obj_align(d->bar_clock, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_align(d->bar_status, LV_ALIGN_RIGHT_MID, -8, 0);
        if (d->backend.width < 200)
            lv_obj_add_flag(d->bar_status, LV_OBJ_FLAG_HIDDEN);
    }
    if (d->system_config.screensaver && !*d->lifecycle_id) {
        d->saver = panel(screen, d->backend.width, d->backend.height, 0x000000);
        if (!d->saver)
            return Q2_ERR_MEMORY;
        d->saver_clock = lv_label_create(d->saver);
        if (!d->saver_clock)
            return Q2_ERR_MEMORY;
        lv_obj_set_size(d->saver_clock,
                        (d->backend.width >= 200 && d->backend.height >= 80) ? 160
                                                                             : (int)d->backend.width - 16,
                        (d->backend.width >= 200 && d->backend.height >= 80) ? 50 : 28);
        lv_obj_set_style_text_font(
            d->saver_clock, (d->backend.width >= 200 && d->backend.height >= 80) ? &d->text_40 : &d->text_20,
            0);
        lv_obj_set_style_text_color(d->saver_clock, lv_color_hex(0x88939f), 0);
        lv_obj_set_style_text_align(d->saver_clock, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(d->saver_clock, LV_LABEL_LONG_CLIP);
        lv_obj_center(d->saver_clock);
        lv_obj_add_flag(d->saver, LV_OBJ_FLAG_HIDDEN);
    }
    status_labels(d);
    return Q2_OK;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    q2_lvgl_t *d = lv_display_get_user_data(display);
    unsigned width = (unsigned)(area->x2 - area->x1 + 1);
    unsigned height = (unsigned)(area->y2 - area->y1 + 1);
    q2_result_t e = d->backend.flush(d->backend.context, (unsigned)area->x1, (unsigned)area->y1, width,
                                     height, (const uint32_t *)pixels, (size_t)width * 4);
    if (e != Q2_OK && d->pending_error == Q2_OK)
        d->pending_error = e;
    /* Always release the draw buffer, including I/O failure; otherwise LVGL can wait forever. */
    lv_display_flush_ready(display);
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) {
    q2_lvgl_t *d = lv_indev_get_user_data(input);
    data->point.x = d->pointer_x;
    data->point.y = d->pointer_y;
    data->state = d->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static q2_result_t take_error(q2_lvgl_t *d) {
    q2_result_t e = d->pending_error;
    d->pending_error = Q2_OK;
    return e;
}
static void clear_objects(q2_lvgl_t *d) {
    /* Clear pressed-object tracking before deleting widgets referenced by the input driver. */
    if (d->scene->instance == d->foreground && !d->scene->layer) {
        d->pressed = false;
        lv_indev_reset(d->input, NULL);
    }
    lv_obj_clean(d->scene->root);
    lv_obj_remove_flag(d->scene->root, LV_OBJ_FLAG_SCROLLABLE);
    d->scene->canvas_height = 0;
    d->scene->wallpaper = NULL;
    memset(d->scene->widgets, 0, sizeof(d->scene->widgets));
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    q2_lvgl_t *d = ctx;
    if (active)
        return Q2_ERR_BUSY;
    active = d;
    d->sleeping = d->pointer_suppressed = false;
    d->scene = &d->scenes[0];
    d->scene->root = d->bar = d->bar_clock = d->bar_status = d->saver = d->saver_clock = NULL;
    lv_init();
    q2_result_t failure = Q2_ERR_MEMORY;
    if (!fonts_create(d))
        goto no_memory;
    /* Decode trusted, bounded package images once, outside the Lua callback budget. */
    for (unsigned i = 0; i < d->image_count; i++) {
        image_t *im = &d->images[i];
        const lv_image_decoder_args_t args = {.no_cache = true};
        if (!im->png.data)
            continue;
        if (lv_image_decoder_open(&im->decoder, &im->png, &args) != LV_RESULT_OK) {
            failure = Q2_ERR_FORMAT;
            goto no_memory;
        }
        lv_draw_buf_to_image(im->decoder.decoded, &im->image);
    }
    d->display = lv_display_create((int32_t)d->backend.width, (int32_t)d->backend.height);
    if (!d->display)
        goto no_memory;
    lv_display_set_user_data(d->display, d);
    lv_display_set_color_format(d->display, LV_COLOR_FORMAT_XRGB8888);
    unsigned rows = d->full_refresh ? d->backend.height : 32u;
    lv_display_set_buffers(d->display, d->buffer, NULL, d->backend.width * rows * 4,
                           d->full_refresh ? LV_DISPLAY_RENDER_MODE_FULL : LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d->display, flush);
    d->input = lv_indev_create();
    if (!d->input)
        goto no_memory;
    lv_indev_set_type(d->input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(d->input, d->display);
    lv_indev_set_user_data(d->input, d);
    lv_indev_set_read_cb(d->input, pointer_read);
    lv_obj_t *screen = lv_display_get_screen_active(d->display);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_text_font(screen, &d->text_20, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    failure = system_create(d, screen);
    if (failure != Q2_OK)
        goto no_memory;
    q2_kernel_time(k, &d->last_tick);
    d->last_activity = d->last_tick;
    d->started = true;
    return Q2_OK;
no_memory:
    /* Failed starts must unwind their own partial state; Kernel will not call stop for them. */
    for (unsigned i = 0; i < d->image_count; i++) {
        if (d->images[i].decoder.decoded)
            lv_image_decoder_close(&d->images[i].decoder);
        memset(&d->images[i].decoder, 0, sizeof(d->images[i].decoder));
    }
    graphics_destroy(d);
    active = NULL;
    return failure;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    (void)k;
    q2_lvgl_t *d = ctx;
    if (!instance)
        return Q2_OK;
    for (unsigned i = 0; i < SCENE_MAX; i++) {
        scene_t *scene = &d->scenes[i];
        if (scene->instance != instance)
            continue;
        d->scene = scene;
        clear_objects(d);
        lv_obj_set_style_bg_color(scene->root, lv_color_hex(0x111827), 0);
    }
    lv_obj_invalidate(lv_display_get_screen_active(d->display));
    lv_refr_now(d->display);
    q2_result_t e = take_error(d);
    if (e == Q2_OK) {
        for (unsigned i = 0; i < SCENE_MAX; i++)
            if (d->scenes[i].instance == instance) {
                lv_obj_t *root = d->scenes[i].root;
                if (i) {
                    lv_obj_delete(root);
                    root = NULL;
                }
                memset(&d->scenes[i], 0, sizeof(scene_t));
                d->scenes[i].root = root;
            }
        if (d->foreground == instance)
            d->foreground = 0;
        if (d->lifecycle_instance == instance)
            d->lifecycle_instance = 0;
    }
    d->scene = &d->scenes[0];
    if (e == Q2_OK)
        e = remote_release(d, instance);
    return e;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    q2_lvgl_t *d = ctx;
    q2_result_t e = Q2_OK;
    for (unsigned i = 0; i < SCENE_MAX && e == Q2_OK; i++)
        e = release_app(k, ctx, d->scenes[i].instance);
    if (e != Q2_OK)
        return e;
    for (unsigned i = 0; i < d->image_count; i++) {
        if (d->images[i].decoder.decoded)
            lv_image_decoder_close(&d->images[i].decoder);
        memset(&d->images[i].decoder, 0, sizeof(d->images[i].decoder));
    }
    graphics_destroy(d);
    d->started = false;
    d->sleeping = d->pointer_suppressed = false;
    active = NULL;
    return Q2_OK;
}
static q2_result_t poll(q2_kernel_t *k, void *ctx) {
    q2_lvgl_t *d = ctx;
    uint64_t now;
    q2_kernel_time(k, &now);
    lv_tick_inc((uint32_t)(now - d->last_tick));
    d->last_tick = now;
    idle_update(d, now);
    remote_poll(d);
    lv_timer_handler();
    return take_error(d);
}
static void layout_scenes(q2_lvgl_t *d) {
    /* Fixed z-order: home/foreground content, screensaver content, then status bar. */
    for (unsigned i = 0; i < SCENE_MAX; i++) {
        scene_t *s = &d->scenes[i];
        if (!s->root || !s->instance)
            continue;
        unsigned h = s->layer == 1 ? d->system_config.status_bar_height : content_height(d);
        lv_obj_set_pos(s->root, 0, s->layer == 1 ? 0 : (int)content_top(d));
        lv_obj_set_size(s->root, (int)d->backend.width, (int)h);
        bool visible = s->layer == 1   ? d->system_config.status_bar
                       : s->layer == 2 ? d->sleeping
                                       : s->instance == d->foreground;
        if (visible)
            lv_obj_remove_flag(s->root, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s->root, LV_OBJ_FLAG_HIDDEN);
    }
    for (int layer = 2; layer >= 1; layer--)
        for (unsigned i = 0; i < SCENE_MAX; i++)
            if (d->scenes[i].root && d->scenes[i].layer == (unsigned)layer)
                lv_obj_move_foreground(d->scenes[i].root);
}
static q2_result_t claim(q2_lvgl_t *d, const q2_caller_t *c) {
    unsigned layer = *d->lifecycle_id && !strcmp(c->app_id, d->lifecycle_id) ? d->lifecycle_layer : 0;
    scene_t *empty = NULL;
    for (unsigned i = 0; i < SCENE_MAX; i++) {
        scene_t *s = &d->scenes[i];
        if (s->instance == c->instance && s->layer == layer) {
            d->scene = s;
            return Q2_OK;
        }
        if (!s->instance && !empty)
            empty = s;
    }
    if (!empty)
        return Q2_ERR_LIMIT;
    if (!empty->root)
        empty->root =
            panel(lv_display_get_screen_active(d->display), d->backend.width, content_height(d), 0x111827);
    if (!empty->root)
        return Q2_ERR_MEMORY;
    empty->instance = c->instance;
    empty->layer = layer;
    strcpy(empty->app_id, c->app_id);
    d->scene = empty;
    if (!strcmp(c->app_id, d->lifecycle_id))
        d->lifecycle_instance = c->instance;
    if (!d->foreground)
        d->foreground = c->instance;
    layout_scenes(d);
    return Q2_OK;
}
static void clicked(lv_event_t *event) {
    q2_lvgl_t *d = lv_event_get_user_data(event);
    lv_obj_t *object = lv_event_get_target_obj(event);
    if (d->sleeping)
        return;
    for (unsigned j = 0; j < SCENE_MAX; j++) {
        scene_t *s = &d->scenes[j];
        if (s->instance != d->foreground || s->layer)
            continue;
        for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++) {
            widget_t *w = &s->widgets[i];
            if (w->object != object)
                continue;
            q2_event_t message = {.instance = s->instance, .value = w->handle, .topic = "ui.click"};
            snprintf(message.text, sizeof(message.text), "%s", w->label ? lv_label_get_text(w->label) : "");
            q2_result_t e = q2_event_post(d->kernel, &message);
            if (e != Q2_OK && d->pending_error == Q2_OK)
                d->pending_error = e;
            return;
        }
    }
}
static q2_result_t make_widget(q2_lvgl_t *d, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out, bool button, bool legacy) {
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    bool styled_label = !button && !legacy && n == 4;
    if ((n != 3 && !(button && n == 4) && !styled_label) || a[0].kind != Q2_INTEGER ||
        a[1].kind != Q2_INTEGER || a[2].kind != Q2_STRING || a[0].as.integer < 0 || a[1].as.integer < 0 ||
        a[0].as.integer >= d->backend.width || a[1].as.integer >= draw_height(d))
        return Q2_ERR_ARGUMENT;
    int x = (int)a[0].as.integer, y = (int)a[1].as.integer;
    unsigned button_width = 160;
    if (button && n == 4) {
        if (a[3].kind != Q2_INTEGER || a[3].as.integer < 48 || a[3].as.integer > 1024)
            return Q2_ERR_ARGUMENT;
        button_width = (unsigned)a[3].as.integer;
    }
    if (button && (x + button_width > d->backend.width || y + 48 > (int)draw_height(d)))
        return Q2_ERR_ARGUMENT;
    unsigned label_width = d->backend.width - (unsigned)x;
    lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT;
    if (styled_label) {
        if (a[3].kind != Q2_STRING)
            return Q2_ERR_ARGUMENT;
        if (!strcmp(a[3].as.string, "center"))
            alignment = LV_TEXT_ALIGN_CENTER;
        else if (!strcmp(a[3].as.string, "right"))
            alignment = LV_TEXT_ALIGN_RIGHT;
        else if (strcmp(a[3].as.string, "left"))
            return Q2_ERR_ARGUMENT;
    }

    widget_t *slot = NULL;
    /* Repeated legacy text calls at a position update a label instead of leaking widgets each frame. */
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++) {
        widget_t *w = &d->scene->widgets[i];
        if (legacy && w->object && w->text_slot && w->x == x && w->y == y) {
            lv_label_set_text(w->label, a[2].as.string);
            return Q2_OK;
        }
        if (!w->object && !slot)
            slot = w;
    }
    if (!slot || d->next_handle == INT32_MAX)
        return Q2_ERR_LIMIT;
    lv_obj_t *screen = d->scene->root;
    lv_obj_t *object = button ? lv_button_create(screen) : lv_label_create(screen);
    if (!object)
        return Q2_ERR_MEMORY;
    lv_obj_t *label = button ? lv_label_create(object) : object;
    if (!label) {
        lv_obj_delete(object);
        return Q2_ERR_MEMORY;
    }
    lv_obj_set_pos(object, x, y);
    if (button) {
        lv_obj_set_size(object, (int)button_width, 48);
        lv_obj_set_style_bg_color(object, lv_color_hex(0x2563eb), 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(0x1e40af), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(object, 12, 0);
        lv_obj_set_style_border_width(object, 0, 0);
        lv_obj_set_style_pad_all(object, 4, 0);
        lv_obj_set_width(label, (int)button_width - 16);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_center(label);
        lv_obj_add_event_cb(object, clicked, LV_EVENT_CLICKED, d);
    } else {
        lv_obj_set_width(label, (int)label_width);
        lv_obj_set_style_text_align(label, alignment, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(label, a[2].as.string);
    *slot = (widget_t){
        .object = object, .label = label, .handle = ++d->next_handle, .x = x, .y = y, .text_slot = legacy};
    if (!legacy) {
        out->kind = Q2_INTEGER;
        out->as.integer = slot->handle;
    }
    return Q2_OK;
}
static q2_result_t label(q2_kernel_t *k, void *d, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    return make_widget(d, c, a, n, out, false, false);
}
static q2_result_t button(q2_kernel_t *k, void *d, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    return make_widget(d, c, a, n, out, true, false);
}
static q2_result_t text(q2_kernel_t *k, void *d, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    return make_widget(d, c, a, n, out, false, true);
}
static q2_result_t clear(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)a;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e == Q2_OK)
        clear_objects(d);
    return e;
}
static q2_result_t background(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 1 || a[0].kind != Q2_INTEGER || a[0].as.integer < 0 || a[0].as.integer > 0xffffff)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e == Q2_OK) {
        if (d->scene->wallpaper) {
            lv_obj_delete(d->scene->wallpaper);
            d->scene->wallpaper = NULL;
        }
        lv_obj_set_style_bg_color(d->scene->root, lv_color_hex((uint32_t)a[0].as.integer), 0);
    }
    return e;
}
static q2_result_t set_text(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 2 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++)
        if (d->scene->widgets[i].object && d->scene->widgets[i].handle == a[0].as.integer) {
            if (!d->scene->widgets[i].label)
                return Q2_ERR_ARGUMENT;
            lv_label_set_text(d->scene->widgets[i].label, a[1].as.string);
            return Q2_OK;
        }
    return Q2_ERR_NOT_FOUND;
}
static image_t *find_image(q2_lvgl_t *d, const q2_caller_t *c, const char *name) {
    for (unsigned i = 0; i < d->image_count; i++)
        if (!strcmp(d->images[i].app_id, c->app_id) && !strcmp(d->images[i].name, name))
            return &d->images[i];
    return NULL;
}
static q2_result_t image(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    q2_lvgl_t *d = ctx;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if ((n != 3 && n != 4) || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_STRING ||
        a[0].as.integer < 0 || a[1].as.integer < 0 || a[0].as.integer >= d->backend.width ||
        a[1].as.integer >= draw_height(d))
        return Q2_ERR_ARGUMENT;
    image_t *im = find_image(d, c, a[2].as.string);
    if (!im)
        return Q2_ERR_NOT_FOUND;
    /* Optional fixed viewport contains the whole icon, preserving aspect ratio and click bounds. */
    if (n == 4 && (a[3].kind != Q2_INTEGER || a[3].as.integer < 1 ||
                   a[3].as.integer > d->backend.width - a[0].as.integer ||
                   a[3].as.integer > draw_height(d) - a[1].as.integer))
        return Q2_ERR_ARGUMENT;

    widget_t *slot = NULL;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++)
        if (!d->scene->widgets[i].object) {
            slot = &d->scene->widgets[i];
            break;
        }
    if (!slot || d->next_handle == INT32_MAX)
        return Q2_ERR_LIMIT;
    lv_obj_t *obj = lv_image_create(d->scene->root);
    if (!obj)
        return Q2_ERR_MEMORY;
    lv_image_set_src(obj, &im->image);
    lv_obj_set_pos(obj, (int)a[0].as.integer, (int)a[1].as.integer);
    if (n == 4) {
        lv_obj_set_size(obj, (int)a[3].as.integer, (int)a[3].as.integer);
        lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, d);
    *slot = (widget_t){.object = obj, .handle = ++d->next_handle};
    out->kind = Q2_INTEGER;
    out->as.integer = slot->handle;
    return Q2_OK;
}
/* One cached URL per instance; two redraws of the same track share the download.
 * Widgets use the package fallback immediately. Late/cancelled responses cannot own a newer URL. */
static q2_result_t image_url(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)out;
    if (n != 2 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_STRING || !a[1].as.string[0])
        return Q2_ERR_ARGUMENT;
    q2_lvgl_t *d = ctx;
    q2_result_t claimed = claim(d, c);
    if (claimed != Q2_OK)
        return claimed;
    widget_t *target = NULL;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; ++i) {
        widget_t *w = &d->scene->widgets[i];
        if (w->object && w->handle == a[0].as.integer && lv_obj_check_type(w->object, &lv_image_class)) {
            target = w;
            break;
        }
    }
    if (!target)
        return Q2_ERR_NOT_FOUND;
    bool allowed = false;
    for (size_t i = 0; i < c->grant_count; ++i)
        if (c->grants[i] && !strcmp(c->grants[i], "network.http"))
            allowed = true;
    if (!allowed)
        return Q2_ERR_PERMISSION;
    const q2_network_binary_t *transport = NULL;
    q2_result_t e = q2_service_get(k, "network.binary", 1, sizeof(*transport), (const void **)&transport);
    if (e != Q2_OK)
        return e;
    remote_t *r = NULL;
    for (unsigned i = 0; i < REMOTE_MAX; ++i) {
        if (d->remote[i].caller.instance == c->instance) {
            r = &d->remote[i];
            break;
        }
        if (!d->remote[i].id)
            r = &d->remote[i];
    }
    if (!r || d->next_handle >= INT32_MAX - 1)
        return Q2_ERR_LIMIT;
    if (r->id && strcmp(r->url, a[1].as.string)) {
        if (remote_used(d, r->id))
            return Q2_ERR_BUSY;
        e = remote_clear(r);
        if (e != Q2_OK)
            return e;
    }
    if (!r->id) {
        r->caller.instance = c->instance;
        strcpy(r->caller.app_id, c->app_id);
        r->caller.grants[0] = "network.http";
        r->caller.grant_count = 1;
        r->id = ++d->next_handle;
        strcpy(r->url, a[1].as.string);
        r->transport = transport;
        q2_kernel_time(k, &r->deadline);
        r->deadline += 5000;
        r->result = Q2_ERR_BUSY;
    }
    target->remote = r->id;
    if (r->result == Q2_OK)
        lv_image_set_src(target->object, &r->image);
    return Q2_OK;
}
/* The returned widget owns both the white quiet-zone panel and the QR canvas.
 * Use integral module sizes; generic size() must not stretch/crop a scannable code. */
static q2_result_t qrcode(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
#if LV_USE_QRCODE
    q2_lvgl_t *d = ctx;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if (n != 4 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_STRING ||
        a[3].kind != Q2_INTEGER || a[0].as.integer < 0 || a[1].as.integer < 0 || a[3].as.integer < 58 ||
        a[3].as.integer > 512 || !a[2].as.string[0] ||
        a[0].as.integer > (int64_t)d->backend.width - a[3].as.integer ||
        a[1].as.integer > (int64_t)draw_height(d) - a[3].as.integer)
        return Q2_ERR_ARGUMENT;
    size_t length = strlen(a[2].as.string);
    int version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, length);
    if (version <= 0)
        return Q2_ERR_LIMIT;
    int modules = qrcodegen_version2size(version), size = (int)a[3].as.integer;
    int scale = size / (modules + 8); // At least four white modules on each edge.
    if (scale < 2)
        return Q2_ERR_ARGUMENT;
    int inner = modules * scale, margin = (size - inner) / 2;
    widget_t *slot = NULL;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++)
        if (!d->scene->widgets[i].object) {
            slot = &d->scene->widgets[i];
            break;
        }
    if (!slot || d->next_handle == INT32_MAX)
        return Q2_ERR_LIMIT;
    lv_obj_t *object = panel(d->scene->root, (unsigned)size, (unsigned)size, 0xffffff);
    if (!object)
        return Q2_ERR_MEMORY;
    lv_obj_t *qr = lv_qrcode_create(object);
    if (!qr) {
        lv_obj_delete(object);
        return Q2_ERR_MEMORY;
    }
    lv_qrcode_set_size(qr, inner);
    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(qr);
    if (!buffer || buffer->header.w != (unsigned)inner || buffer->header.h != (unsigned)inner) {
        lv_obj_delete(object);
        return Q2_ERR_MEMORY;
    }
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    if (lv_qrcode_update(qr, a[2].as.string, (uint32_t)length) != LV_RESULT_OK) {
        lv_obj_delete(object);
        return Q2_ERR_FORMAT;
    }
    lv_obj_set_pos(object, (int)a[0].as.integer, (int)a[1].as.integer);
    lv_obj_set_pos(qr, margin, margin);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    *slot = (widget_t){.object = object, .handle = ++d->next_handle, .qrcode = true};
    out->kind = Q2_INTEGER;
    out->as.integer = slot->handle;
    return Q2_OK;
#else
    (void)ctx;
    (void)c;
    (void)a;
    (void)n;
    (void)out;
    return Q2_ERR_UNSUPPORTED;
#endif
}
static q2_result_t wallpaper(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    image_t *im = find_image(d, c, a[0].as.string);
    if (!im)
        return Q2_ERR_NOT_FOUND;

    lv_obj_t *obj = d->scene->wallpaper;
    if (!obj) {
        obj = lv_image_create(d->scene->root);
        if (!obj)
            return Q2_ERR_MEMORY;
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        d->scene->wallpaper = obj;
    }
    lv_image_set_src(obj, &im->image);
    /* A fixed viewport with COVER preserves aspect ratio and clips excess at the center. */
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, (int)d->backend.width, (int)draw_height(d));
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_COVER);
    lv_obj_move_to_index(obj, 0);
    return Q2_OK;
}
q2_result_t q2_lvgl_resolution(const q2_lvgl_t *d, unsigned *w, unsigned *h) {
    if (!d || !w || !h)
        return Q2_ERR_ARGUMENT;
    *w = d->backend.width;
    *h = d->backend.height;
    return Q2_OK;
}
static q2_result_t resolution(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    const q2_lvgl_t *d = ctx;
    out->kind = Q2_STRING;
    snprintf(out->as.string, sizeof(out->as.string), "%ux%u", d->backend.width, d->backend.height);
    return Q2_OK;
}
static q2_result_t width(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_INTEGER;
    out->as.integer = ((q2_lvgl_t *)ctx)->backend.width;
    return Q2_OK;
}
static q2_result_t height(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_INTEGER;
    q2_result_t e = claim(ctx, c);
    if (e != Q2_OK)
        return e;
    out->as.integer = view_height(ctx);
    return Q2_OK;
}

static const char *preference_keys[] = {
    "status_bar",   "screensaver", "idle_timeout_ms", "clock_move_interval_ms", "lock_enabled", "status_left",
    "status_right", "saver_text",  "unlock_key",      "unlock_action"};
static q2_result_t preference(q2_lvgl_t *d, const char *key, q2_value_t *value, bool write) {
    bool *boolean = NULL;
    uint32_t *number = NULL;
    /* uint32_t need not be unsigned int on Xtensa; never alias the action field through it. */
    uint32_t action = d->preferences.unlock_action;
    char *text = NULL;
    size_t limit = 128;
    if (!strcmp(key, "status_bar"))
        boolean = &d->system_config.status_bar;
    else if (!strcmp(key, "screensaver"))
        boolean = &d->system_config.screensaver;
    else if (!strcmp(key, "lock_enabled"))
        boolean = &d->preferences.lock_enabled;
    else if (!strcmp(key, "idle_timeout_ms"))
        number = &d->system_config.idle_timeout_ms;
    else if (!strcmp(key, "clock_move_interval_ms"))
        number = &d->system_config.clock_move_interval_ms;
    else if (!strcmp(key, "unlock_action"))
        number = &action;
    else if (!strcmp(key, "status_left"))
        text = d->preferences.status_left;
    else if (!strcmp(key, "status_right"))
        text = d->preferences.status_right;
    else if (!strcmp(key, "saver_text"))
        text = d->preferences.saver_text;
    else if (!strcmp(key, "unlock_key")) {
        text = d->preferences.unlock_key;
        limit = 32;
    } else
        return Q2_ERR_NOT_FOUND;
    if (write) {
        if (boolean) {
            if (value->kind != Q2_BOOLEAN)
                return Q2_ERR_ARGUMENT;
            if (boolean == &d->system_config.status_bar && value->as.boolean &&
                d->system_config.status_bar_height + 32 > d->backend.height)
                return Q2_ERR_ARGUMENT;
            *boolean = value->as.boolean;
        } else if (number) {
            int64_t low = number == &action ? 1 : 1000;
            int64_t high = low == 1 ? 3 : 3600000;
            if (value->kind != Q2_INTEGER || value->as.integer < low || value->as.integer > high)
                return Q2_ERR_ARGUMENT;
            *number = (uint32_t)value->as.integer;
            if (number == &action)
                d->preferences.unlock_action = (unsigned)action;
        } else {
            if (value->kind != Q2_STRING || strlen(value->as.string) >= limit)
                return Q2_ERR_ARGUMENT;
            if (limit == 32 && !*value->as.string)
                return Q2_ERR_ARGUMENT;
            strcpy(text, value->as.string);
        }
    } else {
        value->kind = boolean ? Q2_BOOLEAN : number ? Q2_INTEGER : Q2_STRING;
        if (boolean)
            value->as.boolean = *boolean;
        else if (number)
            value->as.integer = *number;
        else
            strcpy(value->as.string, text);
    }
    return Q2_OK;
}
static q2_result_t preferences_load(q2_lvgl_t *d) {
    if (!*d->lifecycle_id || d->preferences_loaded)
        return Q2_OK;
    q2_result_t e = q2_kernel_next_instance(d->kernel, &d->settings_caller.instance);
    if (e != Q2_OK)
        return e;
    strcpy(d->settings_caller.app_id, d->lifecycle_id);
    d->settings_caller.grants[0] = "storage.private";
    d->settings_caller.grants[1] = "input.keys";
    d->settings_caller.grant_count = 2;
    q2_capability_available(d->kernel, "storage.private", &d->persistent);
    if (!d->persistent) {
        d->preferences_loaded = true;
        return Q2_OK;
    }
    for (unsigned i = 0; i < sizeof(preference_keys) / sizeof(*preference_keys); i++) {
        q2_value_t key = {.kind = Q2_STRING}, value, expected;
        snprintf(key.as.string, sizeof(key.as.string), "ui-%s", preference_keys[i]);
        e = q2_kernel_call(d->kernel, &d->settings_caller, "storage", "get", &key, 1, &value);
        if (e == Q2_ERR_NOT_FOUND)
            continue;
        if (e != Q2_OK)
            return e;
        preference(d, preference_keys[i], &expected, false);
        if (expected.kind == Q2_BOOLEAN) {
            if (strcmp(value.as.string, "0") && strcmp(value.as.string, "1"))
                continue;
            expected.as.boolean = !strcmp(value.as.string, "1");
        } else if (expected.kind == Q2_INTEGER) {
            char *end;
            unsigned long v = strtoul(value.as.string, &end, 10);
            if (!*value.as.string || *end || v > 3600000)
                continue;
            expected.as.integer = (int64_t)v;
        } else
            strcpy(expected.as.string, value.as.string);
        /* Invalid old preferences fall back to product defaults; no media is reformatted. */
        (void)preference(d, preference_keys[i], &expected, true);
    }
    /* A changed product key registry must not restore an impossible unlock gesture. */
    if (d->preferences.lock_enabled && registered_unlock_key(d, d->preferences.unlock_key) != Q2_OK)
        d->preferences.lock_enabled = false;
    d->preferences_loaded = true;
    return Q2_OK;
}
static q2_result_t shell_get(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)k;
    (void)c;
    q2_lvgl_t *d = ctx;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = preferences_load(d);
    if (e != Q2_OK)
        return e;
    const char *key = a[0].as.string;
    out->kind = Q2_INTEGER;
    if (!strcmp(key, "last_activity"))
        out->as.integer = (int64_t)d->last_activity;
    else if (!strcmp(key, "revision"))
        out->as.integer = d->revision;
    else if (!strcmp(key, "status_bar_height"))
        out->as.integer = d->system_config.status_bar_height;
    else if (!strcmp(key, "persistent")) {
        out->kind = Q2_BOOLEAN;
        out->as.boolean = d->persistent;
    } else if (!strcmp(key, "time")) {
        out->kind = Q2_STRING;
        if (d->system_status.clock_valid)
            snprintf(out->as.string, sizeof(out->as.string), "%02u:%02u", d->system_status.hour,
                     d->system_status.minute);
        else
            strcpy(out->as.string, "--:--");
    } else if (!strcmp(key, "battery")) {
        out->kind = Q2_STRING;
        if (d->system_status.battery_percent < 0)
            strcpy(out->as.string, "BAT --");
        else
            snprintf(out->as.string, sizeof(out->as.string), "%s%d%%", d->system_status.charging ? "+" : "",
                     d->system_status.battery_percent);
    } else if (!strcmp(key, "wifi") || !strcmp(key, "bt")) {
        out->kind = Q2_STRING;
        strcpy(out->as.string, !strcmp(key, "wifi") ? (d->system_status.wifi_connected ? "WiFi" : "")
                                                    : (d->system_status.bluetooth_connected ? "BT" : ""));
    } else
        return preference(d, key, out, false);
    return Q2_OK;
}
static q2_result_t registered_unlock_key(q2_lvgl_t *d, const char *name) {
    q2_value_t count, out;
    q2_result_t e = q2_kernel_call(d->kernel, &d->settings_caller, "keys", "count", NULL, 0, &count);
    if (e != Q2_OK)
        return e;
    for (int64_t i = 1; i <= count.as.integer; i++) {
        q2_value_t index = {.kind = Q2_INTEGER, .as.integer = i};
        e = q2_kernel_call(d->kernel, &d->settings_caller, "keys", "name", &index, 1, &out);
        if (e != Q2_OK)
            return e;
        if (!strcmp(name, out.as.string))
            return Q2_OK;
    }
    return Q2_ERR_NOT_FOUND;
}
static q2_result_t shell_set(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    q2_lvgl_t *d = ctx;
    if (n != 2 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    bool trusted = !strcmp(c->app_id, d->lifecycle_id);
    for (unsigned i = 0; i < d->factory_count; i++)
        trusted |= !strcmp(c->app_id, d->factory_ids[i]);
    if (!trusted || d->locked)
        return Q2_ERR_PERMISSION;
    q2_result_t loaded = preferences_load(d);
    if (loaded != Q2_OK)
        return loaded;
    if ((!strcmp(a[0].as.string, "unlock_key") && a[1].kind == Q2_STRING) ||
        (!strcmp(a[0].as.string, "lock_enabled") && a[1].kind == Q2_BOOLEAN && a[1].as.boolean)) {
        q2_result_t check =
            registered_unlock_key(d, a[1].kind == Q2_STRING ? a[1].as.string : d->preferences.unlock_key);
        if (check != Q2_OK)
            return check;
    }
    q2_system_ui_config_t old = d->system_config;
    q2_lifecycle_config_t previous = d->preferences;
    q2_value_t value = a[1];
    q2_result_t e = preference(d, a[0].as.string, &value, true);
    if (e != Q2_OK)
        return e;
    if (d->persistent) {
        q2_value_t args[2] = {{.kind = Q2_STRING}, {.kind = Q2_STRING}};
        snprintf(args[0].as.string, sizeof(args[0].as.string), "ui-%.31s", a[0].as.string);
        if (value.kind == Q2_BOOLEAN)
            strcpy(args[1].as.string, value.as.boolean ? "1" : "0");
        else if (value.kind == Q2_INTEGER)
            snprintf(args[1].as.string, sizeof(args[1].as.string), "%lld", (long long)value.as.integer);
        else
            strcpy(args[1].as.string, value.as.string);
        e = q2_kernel_call(k, &d->settings_caller, "storage", "put", args, 2, out);
        if (e != Q2_OK) {
            d->system_config = old;
            d->preferences = previous;
            return e;
        }
    }
    d->revision++;
    q2_kernel_time(k, &d->last_activity);
    layout_scenes(d);
    /* Notify live APPs when usable dimensions change, without recreating any VM. */
    if (old.status_bar != d->system_config.status_bar)
        for (unsigned i = 0; i < SCENE_MAX; i++)
            if (d->scenes[i].instance && !d->scenes[i].layer) {
                q2_event_t event = {.instance = d->scenes[i].instance, .topic = "ui.resize"};
                e = q2_event_post(k, &event);
                if (e != Q2_OK)
                    return e;
            }
    return Q2_OK;
}
static q2_result_t shell_layer(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (strcmp(c->app_id, d->lifecycle_id))
        return Q2_ERR_PERMISSION;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    if (!strcmp(a[0].as.string, "home"))
        d->lifecycle_layer = 0;
    else if (!strcmp(a[0].as.string, "status"))
        d->lifecycle_layer = 1;
    else if (!strcmp(a[0].as.string, "saver"))
        d->lifecycle_layer = 2;
    else
        return Q2_ERR_ARGUMENT;
    return claim(d, c);
}
static q2_result_t shell_present(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a,
                                 size_t n, q2_value_t *out) {
    (void)out;
    q2_lvgl_t *d = ctx;
    if (strcmp(c->app_id, d->lifecycle_id))
        return Q2_ERR_PERMISSION;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    bool sleeping = strcmp(a[0].as.string, "awake") != 0;
    bool locked = !strcmp(a[0].as.string, "locked");
    if (sleeping && !locked && strcmp(a[0].as.string, "saver"))
        return Q2_ERR_ARGUMENT;
    if (d->sleeping && !sleeping)
        q2_kernel_time(k, &d->last_activity);
    d->sleeping = sleeping;
    d->locked = locked;
    d->wake_pending = false;
    if (sleeping)
        lv_indev_reset(d->input, NULL);
    layout_scenes(d);
    return Q2_OK;
}
static q2_result_t keep_awake(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)a;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (c->instance != d->foreground || d->locked || d->sleeping)
        return Q2_ERR_PERMISSION;
    /* A foreground lease must be renewed. Background, exited and locked apps cannot disable the lock. */
    return q2_kernel_time(k, &d->last_activity);
}
static q2_result_t ui_scroll(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 1 || a[0].kind != Q2_INTEGER || a[0].as.integer < 0 || a[0].as.integer > 8192)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if (d->scene->layer)
        return Q2_ERR_PERMISSION;
    d->scene->canvas_height = (unsigned)a[0].as.integer;
    if (d->scene->canvas_height) {
        lv_obj_add_flag(d->scene->root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(d->scene->root, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(d->scene->root, LV_SCROLLBAR_MODE_AUTO);
    } else
        lv_obj_remove_flag(d->scene->root, LV_OBJ_FLAG_SCROLLABLE);
    return Q2_OK;
}
static q2_result_t ui_position(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 3 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_INTEGER)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if (a[1].as.integer < 0 || a[2].as.integer < 0 || a[1].as.integer >= d->backend.width ||
        a[2].as.integer >= draw_height(d))
        return Q2_ERR_ARGUMENT;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++)
        if (d->scene->widgets[i].object && d->scene->widgets[i].handle == a[0].as.integer) {
            widget_t *w = &d->scene->widgets[i];
            if (w->qrcode) {
                lv_obj_update_layout(w->object);
                if (a[1].as.integer + lv_obj_get_width(w->object) > d->backend.width ||
                    a[2].as.integer + lv_obj_get_height(w->object) > draw_height(d))
                    return Q2_ERR_ARGUMENT;
            }
            lv_obj_set_pos(w->object, (int)a[1].as.integer, (int)a[2].as.integer);
            return Q2_OK;
        }
    return Q2_ERR_NOT_FOUND;
}
static q2_result_t ui_style(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 3 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_INTEGER ||
        (a[1].as.integer != 20 && a[1].as.integer != 40) || a[2].as.integer < 0 || a[2].as.integer > 0xffffff)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++) {
        widget_t *w = &d->scene->widgets[i];
        if (w->object && w->handle == a[0].as.integer && w->label) {
            lv_obj_set_style_text_font(w->label, a[1].as.integer == 40 ? &d->text_40 : &d->text_20, 0);
            lv_obj_set_style_text_color(w->label, lv_color_hex((uint32_t)a[2].as.integer), 0);
            return Q2_OK;
        }
    }
    return Q2_ERR_NOT_FOUND;
}
static q2_result_t ui_size(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                           q2_value_t *out) {
    (void)k;
    (void)out;
    q2_lvgl_t *d = ctx;
    if (n != 3 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_INTEGER ||
        a[1].as.integer < 1 || a[1].as.integer > d->backend.width || a[2].as.integer < 1 ||
        a[2].as.integer > 8192)
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    for (unsigned i = 0; i < Q2_UI_OBJECT_MAX; i++) {
        widget_t *w = &d->scene->widgets[i];
        if (w->object && w->handle == a[0].as.integer) {
            if (w->qrcode)
                return Q2_ERR_ARGUMENT;
            lv_obj_set_size(w->object, (int)a[1].as.integer, (int)a[2].as.integer);
            if (w->object == w->label)
                lv_label_set_long_mode(w->label, LV_LABEL_LONG_DOT);
            return Q2_OK;
        }
    }
    return Q2_ERR_NOT_FOUND;
}
static q2_result_t ui_scroll_y(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out) {
    (void)k;
    q2_lvgl_t *d = ctx;
    if (n > 1 || (n && (a[0].kind != Q2_INTEGER || a[0].as.integer < 0 || a[0].as.integer > 8192)))
        return Q2_ERR_ARGUMENT;
    q2_result_t e = claim(d, c);
    if (e != Q2_OK)
        return e;
    if (n) {
        lv_obj_update_layout(d->scene->root);
        lv_obj_scroll_to_y(d->scene->root, (int)a[0].as.integer, LV_ANIM_OFF);
    }
    out->kind = Q2_INTEGER;
    out->as.integer = lv_obj_get_scroll_y(d->scene->root);
    return Q2_OK;
}
static const q2_method_t methods[] = {
    {"ui", "qrcode", "display.ui", qrcode},
    {"ui", "scroll_y", "display.ui", ui_scroll_y},
    {"ui", "size", "display.ui", ui_size},
    {"shell", "get", "system.ui.read", shell_get},
    {"shell", "set", "system.settings", shell_set},
    {"shell", "layer", "system.lifecycle", shell_layer},
    {"shell", "present", "system.lifecycle", shell_present},
    {"shell", "keep_awake", "screensaver.inhibit", keep_awake},
    {"ui", "scroll", "display.ui", ui_scroll},
    {"ui", "position", "display.ui", ui_position},
    {"ui", "style", "display.ui", ui_style},
    {"ui", "image", "display.ui", image},
    {"ui", "image_url", "display.ui", image_url},
    {"ui", "wallpaper", "display.ui", wallpaper},
    {"ui", "resolution", "display.ui", resolution},
    {"ui", "width", "display.ui", width},
    {"ui", "height", "display.ui", height},
    {"display", "text", "display.text", text},
    {"display", "clear", "display.text", clear},
    {"ui", "label", "display.ui", label},
    {"ui", "button", "display.ui", button},
    {"ui", "set_text", "display.ui", set_text},
    {"ui", "clear", "display.ui", clear},
    {"ui", "background", "display.ui", background},
};
q2_result_t q2_lvgl_full_refresh(q2_lvgl_t *d, bool enabled) {
    if (!d)
        return Q2_ERR_ARGUMENT;
    if (d->started)
        return Q2_ERR_STATE;
    if (d->full_refresh == enabled)
        return Q2_OK;
    size_t rows = enabled ? d->backend.height : 32u;
    /* Keep the old allocation usable on failure. Configure only before LVGL retains this pointer. */
    uint32_t *buffer = realloc(d->buffer, (size_t)d->backend.width * rows * sizeof(*buffer));
    if (!buffer)
        return Q2_ERR_MEMORY;
    d->buffer = buffer;
    d->full_refresh = enabled;
    return Q2_OK;
}
q2_result_t q2_lvgl_create(q2_kernel_t *k, const q2_pixel_backend_t *b, q2_lvgl_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!k || !b || b->abi != Q2_PIXEL_ABI || b->size < sizeof(*b) || !b->flush || b->width < 64 ||
        b->width > 1024 || b->height < 64 || b->height > 1024)
        return Q2_ERR_ARGUMENT;
    q2_lvgl_t *d = calloc(1, sizeof(*d));
    if (!d)
        return Q2_ERR_MEMORY;
    d->buffer = malloc((size_t)b->width * 32 * 4);
    if (!d->buffer) {
        free(d);
        return Q2_ERR_MEMORY;
    }
    d->scene = &d->scenes[0];
    d->kernel = k;
    d->backend = *b;
    d->system_status.battery_percent = -1;
    d->component = (q2_component_t){.abi = Q2_COMPONENT_ABI,
                                    .size = sizeof(q2_component_t),
                                    .id = "display",
                                    .required = true,
                                    .after = {"storage_littlefs", "network"},
                                    .context = d,
                                    .start = start,
                                    .stop = stop,
                                    .poll = poll,
                                    .release_app = release_app,
                                    .methods = methods,
                                    .method_count = sizeof(methods) / sizeof(*methods)};
    *out = d;
    return Q2_OK;
}
static unsigned png_word(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
q2_result_t q2_lvgl_register_png(q2_lvgl_t *d, const char *app_id, const char *name, const void *bytes,
                                 size_t size) {
    if (!d || !app_id || strlen(app_id) != 36 || !name || strncmp(name, "assets/", 7) || strlen(name) < 12 ||
        strlen(name) >= 96 || !bytes || size < 33 || size > Q2_UI_PNG_MAX)
        return Q2_ERR_ARGUMENT;
    for (unsigned i = 0; i < SCENE_MAX; i++)
        if (d->scenes[i].instance && !strcmp(d->scenes[i].app_id, app_id))
            return Q2_ERR_STATE;
    /* Flat package keys only. The platform reads files; Lua cannot invent a filesystem path. */
    for (const char *p = name + 7; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-' || *p == '.'))
            return Q2_ERR_ARGUMENT;
    if (strstr(name, "..") || strcmp(name + strlen(name) - 4, ".png"))
        return Q2_ERR_ARGUMENT;
    const unsigned char *p = bytes;
    if (memcmp(p, "\x89PNG\r\n\x1a\n", 8) || png_word(p + 8) != 13 || memcmp(p + 12, "IHDR", 4))
        return Q2_ERR_FORMAT;
    unsigned w = png_word(p + 16), h = png_word(p + 20);
    if (!w || !h || w > 512 || h > 512)
        return Q2_ERR_LIMIT;
    for (unsigned i = 0; i < d->image_count; i++)
        if (!strcmp(d->images[i].name, name) && !strcmp(d->images[i].app_id, app_id))
            return Q2_ERR_EXISTS;
    unsigned slot = 0;
    while (slot < d->image_count && d->images[slot].png.data)
        slot++;
    if (slot == Q2_UI_IMAGE_MAX || w * h > 2097152 - d->image_pixels)
        return Q2_ERR_LIMIT;
    unsigned char *copy = malloc(size);
    if (!copy)
        return Q2_ERR_MEMORY;
    memcpy(copy, bytes, size);
    image_t *im = &d->images[slot];
    if (slot == d->image_count)
        d->image_count++;
    strcpy(im->name, name);
    strcpy(im->app_id, app_id);
    im->png = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .w = w, .h = h},
        .data_size = (uint32_t)size,
        .data = copy};
    d->image_pixels += w * h;
    if (d->started) {
        const lv_image_decoder_args_t args = {.no_cache = true};
        if (lv_image_decoder_open(&im->decoder, &im->png, &args) != LV_RESULT_OK) {
            if (slot + 1 == d->image_count)
                d->image_count--;
            d->image_pixels -= w * h;
            free(copy);
            memset(im, 0, sizeof(*im));
            return Q2_ERR_FORMAT;
        }
        lv_draw_buf_to_image(im->decoder.decoded, &im->image);
    }
    return Q2_OK;
}
q2_result_t q2_lvgl_clear_images(q2_lvgl_t *d) {
    if (!d)
        return Q2_ERR_ARGUMENT;
    for (unsigned i = 0; i < SCENE_MAX; i++)
        if (d->scenes[i].instance)
            return Q2_ERR_BUSY;
    if (d->started)
        lv_image_cache_drop(NULL);
    for (unsigned i = 0; i < d->image_count; i++) {
        if (d->images[i].decoder.decoded)
            lv_image_decoder_close(&d->images[i].decoder);
        free((void *)d->images[i].png.data);
    }
    memset(d->images, 0, sizeof(d->images));
    d->image_count = d->image_pixels = 0;
    return Q2_OK;
}
q2_result_t q2_lvgl_component(q2_lvgl_t *d, const q2_component_t **out) {
    if (!d || !out)
        return Q2_ERR_ARGUMENT;
    *out = &d->component;
    return Q2_OK;
}
q2_result_t q2_lvgl_system_configure(q2_lvgl_t *d, const q2_system_ui_config_t *config) {
    if (!d || !config)
        return Q2_ERR_ARGUMENT;
    if (d->started)
        return Q2_ERR_STATE;
    if ((config->status_bar && (config->status_bar_height < 24 || config->status_bar_height > 64 ||
                                config->status_bar_height + 32 > d->backend.height)) ||
        (config->screensaver && (!config->idle_timeout_ms || !config->clock_move_interval_ms)))
        return Q2_ERR_ARGUMENT;
    d->system_config = *config;
    return Q2_OK;
}
q2_result_t q2_lvgl_system_status(q2_lvgl_t *d, const q2_system_ui_status_t *status) {
    if (!d || !status || status->hour > 23 || status->minute > 59 || status->battery_percent < -1 ||
        status->battery_percent > 100)
        return Q2_ERR_ARGUMENT;
    /* Explicit field comparison avoids struct-padding reads and unnecessary redraws every frame. */
    q2_system_ui_status_t *old = &d->system_status;
    bool changed = old->clock_valid != status->clock_valid || old->hour != status->hour ||
                   old->minute != status->minute || old->battery_percent != status->battery_percent ||
                   old->charging != status->charging || old->wifi_connected != status->wifi_connected ||
                   old->bluetooth_connected != status->bluetooth_connected;
    *old = *status;
    if (changed && d->started && !*d->lifecycle_id)
        status_labels(d);
    return Q2_OK;
}
q2_result_t q2_lvgl_system_state(q2_lvgl_t *d, q2_system_ui_state_t *out) {
    if (!d || !out)
        return Q2_ERR_ARGUMENT;
    *out = (q2_system_ui_state_t){d->sleeping,    d->backend.width, content_height(d),
                                  content_top(d), d->locked,        *d->lifecycle_id != 0};
    return Q2_OK;
}
q2_result_t q2_lvgl_activity(q2_lvgl_t *d, bool *out_consumed) {
    if (!d || !out_consumed)
        return Q2_ERR_ARGUMENT;
    if (!d->started)
        return Q2_ERR_STATE;
    uint64_t now;
    q2_result_t e = q2_kernel_time(d->kernel, &now);
    if (e != Q2_OK)
        return e;
    idle_update(d, now);
    *out_consumed = d->sleeping;
    d->last_activity = now;
    if (d->sleeping && *d->lifecycle_id) {
        if (!d->wake_pending) {
            q2_event_t event = {.instance = d->lifecycle_instance, .topic = "shell.wake"};
            e = q2_event_post(d->kernel, &event);
            if (e != Q2_OK)
                return e;
            d->wake_pending = true;
        }
        return Q2_OK;
    }
    if (d->sleeping) {
        d->sleeping = false;
        lv_obj_add_flag(d->saver, LV_OBJ_FLAG_HIDDEN);
        lv_indev_reset(d->input, NULL);
        d->pressed = false;
    }
    return Q2_OK;
}
q2_result_t q2_lvgl_pointer(q2_lvgl_t *d, int x, int y, bool pressed) {
    if (!d || x < 0 || y < 0 || (unsigned)x >= d->backend.width || (unsigned)y >= d->backend.height)
        return Q2_ERR_ARGUMENT;
    if (!d->started)
        return Q2_ERR_STATE;
    if (pressed || d->pressed || d->pointer_suppressed) {
        bool consumed = false;
        q2_result_t e = q2_lvgl_activity(d, &consumed);
        if (e != Q2_OK)
            return e;
        if (consumed)
            d->pointer_suppressed = true;
    }
    if (d->pointer_suppressed) {
        if (!pressed)
            d->pointer_suppressed = false;
        return take_error(d);
    }
    d->pointer_x = x;
    d->pointer_y = y;
    d->pressed = pressed;
    lv_obj_update_layout(lv_display_get_screen_active(d->display));
    lv_indev_read(d->input);
    return take_error(d);
}
q2_result_t q2_lvgl_destroy(q2_lvgl_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    if ((*ref)->started)
        return Q2_ERR_BUSY;
    for (unsigned i = 0; i < (*ref)->image_count; i++)
        free((void *)(*ref)->images[i].png.data);
    free((*ref)->buffer);
    free(*ref);
    *ref = NULL;
    return Q2_OK;
}

q2_result_t q2_lvgl_lifecycle(q2_lvgl_t *d, const char *id, const q2_lifecycle_config_t *config) {
    if (!d || !id || strlen(id) != 36 || !config || config->unlock_action < 1 || config->unlock_action > 3 ||
        !memchr(config->status_left, 0, 128) || !memchr(config->status_right, 0, 128) ||
        !memchr(config->saver_text, 0, 128) || !memchr(config->unlock_key, 0, 32) || !*config->unlock_key)
        return Q2_ERR_ARGUMENT;
    if (d->started)
        return Q2_ERR_STATE;
    strcpy(d->lifecycle_id, id);
    d->preferences = *config;
    return Q2_OK;
}
q2_result_t q2_lvgl_factory(q2_lvgl_t *d, const char *id) {
    if (!d || !id || strlen(id) != 36)
        return Q2_ERR_ARGUMENT;
    if (d->started)
        return Q2_ERR_STATE;
    if (d->factory_count == Q2_RUNTIME_MAX)
        return Q2_ERR_LIMIT;
    for (unsigned i = 0; i < d->factory_count; i++)
        if (!strcmp(id, d->factory_ids[i]))
            return Q2_ERR_EXISTS;
    strcpy(d->factory_ids[d->factory_count++], id);
    return Q2_OK;
}
q2_result_t q2_lvgl_focus(q2_lvgl_t *d, uint64_t instance) {
    if (!d || !instance)
        return Q2_ERR_ARGUMENT;
    if (!d->started)
        return Q2_ERR_STATE;
    if (d->foreground != instance) {
        d->foreground = instance;
        lv_indev_reset(d->input, NULL);
        d->pressed = false;
        layout_scenes(d);
    }
    return Q2_OK;
}
q2_result_t q2_lvgl_clear_app_images(q2_lvgl_t *d, const char *id) {
    if (!d || !id)
        return Q2_ERR_ARGUMENT;
    for (unsigned i = 0; i < SCENE_MAX; i++)
        if (d->scenes[i].instance && !strcmp(d->scenes[i].app_id, id))
            return Q2_ERR_BUSY;
    for (unsigned i = 0; i < d->image_count; i++) {
        image_t *im = &d->images[i];
        if (strcmp(im->app_id, id))
            continue;
        if (d->started)
            lv_image_cache_drop(&im->image);
        if (im->decoder.decoded)
            lv_image_decoder_close(&im->decoder);
        d->image_pixels -= im->png.header.w * im->png.header.h;
        free((void *)im->png.data);
        memset(im, 0, sizeof(*im));
    }
    return Q2_OK;
}
q2_result_t q2_lvgl_scroll(q2_lvgl_t *d, int delta) {
    if (!d)
        return Q2_ERR_ARGUMENT;
    bool consumed = false;
    q2_result_t e = q2_lvgl_activity(d, &consumed);
    if (e != Q2_OK || consumed)
        return e;
    for (unsigned i = 0; i < SCENE_MAX; i++)
        if (d->scenes[i].instance == d->foreground && !d->scenes[i].layer) {
            lv_obj_scroll_by(d->scenes[i].root, 0, delta, LV_ANIM_OFF);
            return take_error(d);
        }
    return Q2_OK;
}
