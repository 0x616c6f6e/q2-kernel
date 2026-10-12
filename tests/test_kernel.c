#include "device/dcd.h"
#include "q2/display.h"
#include "q2/kernel.h"
#include "q2/runtime.h"
#include "q2/sqlite.h"
#include "q2/storage.h"
#include "q2/usb.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                     \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static size_t live;
static void *allocate(void *ctx, size_t n) {
    (void)ctx;
    void *p = malloc(n);
    if (p)
        live++;
    return p;
}
static void release(void *ctx, void *p) {
    (void)ctx;
    if (p)
        live--;
    free(p);
}
static uint64_t now(void *ctx) {
    (void)ctx;
    return 1000;
}
static q2_port_t port = {Q2_PORT_ABI, sizeof(q2_port_t), NULL, allocate, release, now};
static q2_result_t success(q2_kernel_t *k, void *ctx) {
    (void)k;
    (void)ctx;
    return Q2_OK;
}
static q2_result_t fail(q2_kernel_t *k, void *ctx) {
    (void)k;
    (void)ctx;
    return Q2_ERR_IO;
}
static bool fail_clear;
static q2_result_t clear_display(void *ctx) {
    (void)ctx;
    return fail_clear ? Q2_ERR_IO : Q2_OK;
}
static q2_result_t draw_text(void *ctx, unsigned x, unsigned y, const char *text) {
    (void)ctx;
    (void)x;
    (void)y;
    (void)text;
    return Q2_OK;
}
static q2_result_t malformed_output(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a,
                                    size_t n, q2_value_t *out) {
    (void)k;
    (void)ctx;
    (void)c;
    (void)a;
    (void)n;
    out->kind = Q2_STRING;
    memset(out->as.string, 'x', sizeof(out->as.string));
    return Q2_OK;
}
static void contracts(void) {
    q2_kernel_t *k = NULL;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    const unsigned api = 42;
    const q2_service_t service = {"probe", 1, sizeof(api), &api};
    const q2_method_t method = {"probe", "bad", "test.probe", malformed_output};
    const q2_component_t probe = {.abi = 1,
                                  .size = sizeof(probe),
                                  .id = "probe",
                                  .start = success,
                                  .stop = success,
                                  .services = &service,
                                  .service_count = 1,
                                  .methods = &method,
                                  .method_count = 1};
    CHECK(q2_component_register(k, &probe) == Q2_OK);
    q2_display_backend_t backend = {1, sizeof(backend), NULL, 100, 100, clear_display, draw_text};
    q2_display_t *display = NULL;
    const q2_component_t *component;
    CHECK(q2_display_create(&backend, &display) == Q2_OK);
    CHECK(q2_display_component(display, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    const void *found = NULL;
    CHECK(q2_service_get(k, "probe", 1, sizeof(api), &found) == Q2_OK && found == &api);
    CHECK(q2_service_get(k, "probe", 2, sizeof(api), &found) == Q2_ERR_UNSUPPORTED && found == NULL);
    q2_caller_t caller = {.instance = 1, .grants = {"test.probe", "display.text"}, .grant_count = 2};
    q2_value_t out;
    CHECK(q2_kernel_call(k, &caller, "probe", "bad", NULL, 0, &out) == Q2_ERR_COMPONENT);
    CHECK(out.kind == Q2_NIL);
    q2_value_t bad = {.kind = Q2_STRING};
    memset(bad.as.string, 'x', sizeof(bad.as.string));
    CHECK(q2_kernel_call(k, &caller, "probe", "bad", &bad, 1, &out) == Q2_ERR_ARGUMENT);
    caller.grant_count = 0;
    CHECK(q2_kernel_call(k, &caller, "display", "clear", NULL, 0, &out) == Q2_ERR_PERMISSION);
    caller.grant_count = 2;
    CHECK(q2_kernel_call(k, &caller, "display", "clear", NULL, 0, &out) == Q2_OK);
    caller.instance = 2;
    CHECK(q2_kernel_call(k, &caller, "display", "clear", NULL, 0, &out) == Q2_ERR_BUSY);
    fail_clear = true;
    CHECK(q2_kernel_release_app(k, 1) == Q2_ERR_IO);
    CHECK(q2_kernel_destroy(&k) == Q2_ERR_IO && k != NULL);
    fail_clear = false;
    CHECK(q2_kernel_release_app(k, 1) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_display_destroy(&display) == Q2_OK && live == 0);
}
static void graph(void) {
    q2_kernel_t *k = NULL;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    q2_component_t a = {
        .abi = 1, .size = sizeof(a), .id = "a", .start = success, .stop = success, .needs = {"b"}};
    CHECK(q2_component_register(k, &a) == Q2_OK);
    CHECK(q2_component_register(k, &a) == Q2_ERR_EXISTS);
    CHECK(q2_kernel_boot(k) == Q2_ERR_DEPENDENCY);
    q2_component_t b = {
        .abi = 1, .size = sizeof(b), .id = "b", .start = fail, .stop = success, .needs = {"a"}};
    CHECK(q2_component_register(k, &b) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_ERR_CYCLE);
    CHECK(q2_kernel_destroy(&k) == Q2_OK && k == NULL && live == 0);
    b.needs[0] = NULL;
    a.required = true;
    q2_component_t c = {
        .abi = 1, .size = sizeof(c), .id = "c", .start = success, .stop = success, .after = {"a"}};
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_component_register(k, &c) == Q2_OK);
    CHECK(q2_component_register(k, &a) == Q2_OK);
    CHECK(q2_component_register(k, &b) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_ERR_COMPONENT);
    q2_component_status_t s;
    CHECK(q2_component_status(k, 0, &s) == Q2_OK && s.state == Q2_READY);
    CHECK(q2_component_status(k, 1, &s) == Q2_OK && s.state == Q2_BLOCKED);
    CHECK(q2_component_status(k, 2, &s) == Q2_OK && s.state == Q2_FAILED);
    CHECK(q2_kernel_boot(k) == Q2_ERR_STATE);
    CHECK(q2_component_register(k, &a) == Q2_ERR_STATE);
    q2_event_t event = {.topic = "test"};
    for (unsigned i = 0; i < Q2_EVENT_MAX; i++)
        CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_event_post(k, &event) == Q2_ERR_LIMIT);
    CHECK(q2_kernel_poll(k, 5) == Q2_OK);
    q2_kernel_status_t state;
    CHECK(q2_kernel_status(k, &state) == Q2_OK && state.queued == 27 && state.dropped == 1);
    CHECK(q2_kernel_poll(k, 100) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK && live == 0);
}
static const char manifest[] = "{\"format\":2,\"id\":\"fd4f1e6c-2e93-4e90-a913-36cc84d06793\",\"name\":"
                               "\"test\",\"version\":\"1.2.3-rc.1+build.5\",\"type\":\"app\",\"entry\":"
                               "\"main.lua\",\"api\":2,\"capabilities\":{\"required\":[\"system.info\","
                               "\"storage.private\"],\"optional\":[\"storage.fatfs\",\"storage.database\","
                               "\"usb.cdc\"]},\"resources\":{\"lua_heap_kib\":64}}";
static const char *grants[] = {"system.info", "storage.private", "storage.fatfs", "storage.database",
                               "usb.cdc"};
static void manifests(void) {
    q2_manifest_t m;
    CHECK(q2_manifest_parse(manifest, strlen(manifest), &m) == Q2_OK);
    CHECK(m.required_count == 2 && m.optional_count == 3 && m.heap_bytes == 65536);
    char data[4096];
    CHECK(m.supported_resolution_count == 0);
    snprintf(data, sizeof(data), "%.*s,\"supported_resolutions\":[\"64x64\",\"1024x1024\",\"502x410\"]}",
             (int)strlen(manifest) - 1, manifest);
    CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_OK);
    CHECK(m.supported_resolution_count == 3 && !strcmp(m.supported_resolutions[2], "502x410"));
    const char *bad_sizes[] = {"null", "[]", "\"502x410\"", "[true]", "[123]", "[\"0502x410\"]",
                              "[\"502X410\"]", "[\"63x410\"]", "[\"1025x410\"]", "[\"502x410\\n\"]",
                              "[\"502x410\",\"502x410\"]", "[\"../../secret\"]",
                              "[\"999999999999x410\"]",
                              "[\"64x64\",\"65x64\",\"66x64\",\"67x64\",\"68x64\",\"69x64\","
                              "\"70x64\",\"71x64\",\"72x64\",\"73x64\",\"74x64\",\"75x64\","
                              "\"76x64\",\"77x64\",\"78x64\",\"79x64\",\"80x64\"]"};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(*bad_sizes); i++) {
        snprintf(data, sizeof(data), "%.*s,\"supported_resolutions\":%s}",
                 (int)strlen(manifest) - 1, manifest, bad_sizes[i]);
        CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_ERR_FORMAT);
        CHECK(m.supported_resolution_count == 0 && !m.id[0]);
    }
    strcpy(data, manifest);
    char *v = strstr(data, "1.2.3-rc.1+build.5");
    v[0] = '0';
    v[1] = '1';
    CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_ERR_FORMAT);
    strcpy(data, manifest);
    strstr(data, "fd4f1e6c")[0] = 'F';
    CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_ERR_FORMAT);
    snprintf(data, sizeof(data), "%sx", manifest);
    CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_ERR_FORMAT);
    strcpy(data, manifest);
    strstr(data, "\"api\":2")[6] = '1';
    CHECK(q2_manifest_parse(data, strlen(data), &m) == Q2_ERR_FORMAT);
}
typedef struct {
    unsigned char *data;
    size_t size;
    bool flash;
} medium_t;
static q2_result_t read_media(void *ctx, uint64_t offset, void *data, size_t n) {
    medium_t *m = ctx;
    if (offset > m->size || n > m->size - offset)
        return Q2_ERR_IO;
    memcpy(data, m->data + (size_t)offset, n);
    return Q2_OK;
}
static q2_result_t write_media(void *ctx, uint64_t offset, const void *data, size_t n) {
    medium_t *m = ctx;
    if (offset > m->size || n > m->size - offset)
        return Q2_ERR_IO;
    if (m->flash)
        for (size_t i = 0; i < n; i++)
            if ((m->data[offset + i] & ((const unsigned char *)data)[i]) != ((const unsigned char *)data)[i])
                return Q2_ERR_IO;
    memcpy(m->data + (size_t)offset, data, n);
    return Q2_OK;
}
static q2_result_t erase_media(void *ctx, uint64_t offset, size_t n) {
    medium_t *m = ctx;
    if (offset > m->size || n > m->size - offset)
        return Q2_ERR_IO;
    memset(m->data + (size_t)offset, 255, n);
    return Q2_OK;
}
static q2_result_t sync_media(void *ctx) {
    (void)ctx;
    return Q2_OK;
}
static unsigned released;
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    (void)k;
    (void)ctx;
    CHECK(instance > 0);
    released++;
    return Q2_OK;
}
static q2_result_t run(q2_lua_runtime_t *r, const char *source) {
    return q2_lua_start(r, manifest, strlen(manifest), source, strlen(source), grants, 5);
}
extern unsigned q2_test_usb_transfers;
extern uint8_t q2_test_usb_last[256];
extern uint16_t q2_test_usb_length;
static void integration(void) {
    q2_kernel_t *k = NULL;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    medium_t flash = {malloc(256 * 1024), 256 * 1024, true},
             disk = {malloc(2 * 1024 * 1024), 2 * 1024 * 1024, false};
    CHECK(flash.data && disk.data);
    memset(flash.data, 255, flash.size);
    memset(disk.data, 0, disk.size);
    q2_block_device_t device = {1,  sizeof(device), &flash,      4096,        64,        16,
                                16, read_media,     write_media, erase_media, sync_media};
    q2_storage_t *lfs = NULL, *fat = NULL;
    CHECK(q2_littlefs_create(k, &device, &lfs) == Q2_OK);
    CHECK(q2_storage_format(lfs) == Q2_OK);
    device.context = &disk;
    device.block_size = 512;
    device.block_count = 4096;
    CHECK(q2_fatfs_create(k, &device, &fat) == Q2_OK);
    CHECK(q2_storage_format(fat) == Q2_OK);
    const q2_component_t *component;
    CHECK(q2_storage_component(lfs, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_storage_component(fat, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    q2_sqlite_t *db = NULL;
    CHECK(q2_sqlite_create(&db) == Q2_OK);
    CHECK(q2_sqlite_component(db, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    q2_usb_t *usb = NULL;
    q2_usb_config_t usb_config = {0xcafe, 0x4000, 0};
    CHECK(q2_usb_create(&usb_config, &usb) == Q2_OK);
    CHECK(q2_usb_component(usb, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    q2_component_t monitor = {.abi = 1,
                              .size = sizeof(monitor),
                              .id = "test_monitor",
                              .start = success,
                              .stop = success,
                              .release_app = release_app};
    CHECK(q2_component_register(k, &monitor) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    for (size_t i = 0; i < 5; i++) {
        q2_component_status_t s;
        CHECK(q2_component_status(k, i, &s) == Q2_OK);
        if (s.state != Q2_READY)
            fprintf(stderr, "component %s error %d\n", s.id, s.error);
        CHECK(s.state == Q2_READY);
    }
    CHECK(q2_storage_format(lfs) == Q2_ERR_BUSY);
    /* Exercise the actual USB control stack using a test controller, not fake
     * class APIs. */
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    CHECK(q2_kernel_poll(k, 8) == Q2_OK);
    uint8_t setup[8] = {0x80, TUSB_REQ_GET_DESCRIPTOR, 0, TUSB_DESC_DEVICE, 0, 0, 18, 0};
    dcd_event_setup_received(0, setup, false);
    CHECK(q2_kernel_poll(k, 8) == Q2_OK);
    CHECK(q2_test_usb_transfers > 0 && q2_test_usb_length == 18 && q2_test_usb_last[1] == TUSB_DESC_DEVICE);
    q2_lua_runtime_t *lua = NULL, *other = NULL;
    CHECK(q2_lua_create(k, &lua) == Q2_OK);
    CHECK(q2_lua_create(k, &other) == Q2_OK);
    CHECK(q2_lua_destroy(&other) == Q2_OK);
    CHECK(q2_lua_start(lua, manifest, strlen(manifest), "return {}", 9, NULL, 0) == Q2_ERR_PERMISSION);
    const char *script = "local q=require('q2');return {on_start=function() "
                         "assert(q.os.info().name=='Q2-Kernel');assert(io==nil and os==nil and "
                         "package==nil and debug==nil and pcall==nil and setmetatable==nil);"
                         "assert(q.storage.put('test','littlefs'));assert(q.storage.get('test')=='"
                         "littlefs');"
                         "assert(q.fatfs.put('test','fatfs'));assert(q.fatfs.get('test')=='fatfs')"
                         ";"
                         "assert(q.sqlite.put('test','sqlite'));assert(q.sqlite.get('test')=='"
                         "sqlite');"
                         "local v,e=q.storage.get('../bad');assert(v==nil and "
                         "e.code=='invalid_argument');"
                         "v,e=q.sqlite.get('../bad');assert(v==nil and e.code=='invalid_argument');"
                         "local available,allowed=q.os.capability('wifi.manage');assert(not "
                         "available and not allowed);"
                         "assert(q.usb.connected()==false) "
                         "end,on_event=function(e)assert(q.storage.put('event',e.text))end}";
    q2_result_t e = run(lua, script);
    q2_app_status_t app;
    q2_lua_status(lua, &app);
    if (e != Q2_OK)
        fprintf(stderr, "Lua: %d %s\n", e, app.error);
    CHECK(e == Q2_OK);
    q2_event_t event = {.instance = app.instance, .topic = "test", .text = "delivered"};
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 8) == Q2_OK);
    CHECK(q2_lua_stop(lua) == Q2_OK && released == 1);
    CHECK(run(lua, "local q=require('q2');return "
                   "{on_start=function()assert(q.storage.get('event')=='"
                   "delivered')end}") == Q2_OK);
    CHECK(q2_lua_stop(lua) == Q2_OK);
    CHECK(q2_lua_start(
              lua, manifest, strlen(manifest),
              "local q=require('q2');return {on_start=function() local v,e=q.sqlite.get('test');"
              "assert(v==nil and e.code=='permission_denied') end}",
              strlen("local q=require('q2');return {on_start=function() local v,e=q.sqlite.get('test');"
                     "assert(v==nil and e.code=='permission_denied') end}"),
              grants, 2) == Q2_OK);
    CHECK(q2_lua_stop(lua) == Q2_OK);
    char second_manifest[4096];
    /* Two existing keys plus thirty new keys fill the private quota. Updating an
     * existing key must still succeed while a thirty-third key is rejected. */
    CHECK(run(lua, "local q=require('q2');local i=0;return {on_tick=function() "
                   "i=i+1;if i<=30 then assert(q.storage.put('quota_'..i,'value')) "
                   "else local v,e=q.storage.put('overflow','value');"
                   "assert(v==nil and e.code=='quota_exceeded');"
                   "assert(q.storage.put('quota_1','updated')) end end}") == Q2_OK);
    for (unsigned i = 0; i < 31; ++i)
        CHECK(q2_kernel_poll(k, 8) == Q2_OK);
    CHECK(q2_lua_stop(lua) == Q2_OK);
    strcpy(second_manifest, manifest);
    strstr(second_manifest, "fd4f1e6c")[0] = 'a';
    const char *isolation = "local q=require('q2');return {on_start=function() "
                            "local v,e=q.storage.get('test');assert(v==nil and e.code=='not_found');"
                            "v,e=q.fatfs.get('test');assert(v==nil and e.code=='not_found');"
                            "v,e=q.sqlite.get('test');assert(v==nil and e.code=='not_found') end}";
    CHECK(q2_lua_start(lua, second_manifest, strlen(second_manifest), isolation, strlen(isolation), grants,
                       5) == Q2_OK);
    CHECK(q2_lua_stop(lua) == Q2_OK);
    CHECK(run(lua, "local t={} for i=1,100000 do t[i]=i end return {}") == Q2_ERR_MEMORY);
    CHECK(q2_lua_status(lua, &app) == Q2_OK && !app.running && app.heap_bytes == 0);
    CHECK(run(lua, "while true do end") == Q2_ERR_RUNTIME);
    CHECK(q2_lua_status(lua, &app) == Q2_OK && !app.running && app.heap_bytes == 0);
    CHECK(run(lua, "return {on_event=function()error('bad event')end}") == Q2_OK);
    q2_lua_status(lua, &app);
    event.instance = app.instance - 1;
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 8) == Q2_OK);
    event.instance = app.instance;
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 8) == Q2_ERR_RUNTIME);
    CHECK(q2_lua_status(lua, &app) == Q2_OK && !app.running);
    CHECK(q2_kernel_destroy(&k) == Q2_ERR_BUSY);
    CHECK(q2_lua_destroy(&lua) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK && live == 0);
    /* Re-mount existing filesystems without formatting to prove persisted
     * content. */
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_storage_component(lfs, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_storage_component(fat, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_lua_create(k, &lua) == Q2_OK);
    CHECK(run(lua, "local q=require('q2');return "
                   "{on_start=function()assert(q.storage.get('test')=='littlefs')"
                   ";assert(q.fatfs.get('test')=='fatfs')end}") == Q2_OK);
    CHECK(q2_lua_destroy(&lua) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK && live == 0);
    CHECK(q2_storage_destroy(&lfs) == Q2_OK);
    CHECK(q2_storage_destroy(&fat) == Q2_OK);
    CHECK(q2_sqlite_destroy(&db) == Q2_OK);
    CHECK(q2_usb_destroy(&usb) == Q2_OK);
    free(flash.data);
    free(disk.data);
}
int main(void) {
    contracts();
    graph();
    manifests();
    integration();
    printf("PASS: %u Q2-Kernel checks (real Lua/LittleFS/FatFs/SQLite/TinyUSB)\n", checks);
    return 0;
}
