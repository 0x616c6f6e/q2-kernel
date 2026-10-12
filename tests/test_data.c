#include "q2/data.h"
#include "q2/keys.h"
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, received;
static uint64_t clock_ms;
static q2_event_t last;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                  \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
static void *allocate(void *c, size_t n) {
    (void)c;
    return malloc(n);
}
static void release(void *c, void *p) {
    (void)c;
    free(p);
}
static uint64_t now(void *c) {
    (void)c;
    return clock_ms;
}
static q2_result_t event(void *c, const q2_event_t *e) {
    (void)c;
    if (e->has_data) {
        received++;
        last = *e;
    }
    return Q2_OK;
}
static q2_result_t noop(void *c) {
    (void)c;
    return Q2_OK;
}
static q2_value_t text(const char *s) {
    q2_value_t v = {.kind = Q2_STRING};
    strcpy(v.as.string, s);
    return v;
}
static q2_result_t call(q2_kernel_t *k, const q2_caller_t *c, const char *method, const char *name,
                        q2_value_t *out) {
    q2_value_t a = text(name);
    return q2_kernel_call(k, c, "data", method, &a, 1, out);
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_kernel_t *k = NULL;
    q2_data_t *d = NULL;
    q2_keys_t *keys = NULL;
    const q2_component_t *component;
    int owner, other;
    unsigned battery, charging, edge, title;
    q2_value_t out;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_data_create(k, &d) == Q2_OK);
    q2_data_spec_t spec = {.name = "power.battery.percent",
                           .capability = "power.read",
                           .unit = "percent",
                           .type = Q2_INTEGER,
                           .bounded = true,
                           .minimum = 0,
                           .maximum = 100,
                           .ttl_ms = 100};
    CHECK(q2_data_register(d, &owner, &spec, &battery) == Q2_OK);
    CHECK(q2_data_register(d, &other, &spec, &edge) == Q2_ERR_EXISTS);
    spec.name = "power.battery.charging";
    spec.type = Q2_BOOLEAN;
    spec.bounded = false;
    spec.ttl_ms = 0;
    CHECK(q2_data_register(d, &owner, &spec, &charging) == Q2_OK);
    spec.name = "test.edge";
    spec.kind = Q2_DATA_EVENT;
    spec.type = Q2_INTEGER;
    spec.foreground_only = true;
    CHECK(q2_data_register(d, &owner, &spec, &edge) == Q2_OK);
    for (unsigned i = 0; i < 17; i++) {
        char name[32];
        snprintf(name, sizeof(name), "test.state%u", i);
        spec.name = name;
        spec.kind = Q2_DATA_STATE;
        spec.foreground_only = false;
        unsigned h;
        CHECK(q2_data_register(d, &owner, &spec, &h) == Q2_OK);
    }
    spec = (q2_data_spec_t){.name = "test.title", .capability = "power.read", .unit = "", .type = Q2_STRING};
    CHECK(q2_data_register(d, &owner, &spec, &title) == Q2_OK);
    CHECK(q2_keys_create(k, &keys) == Q2_OK);
    CHECK(q2_keys_register(keys, "POWER") == Q2_OK);
    CHECK(q2_keys_data(keys, d) == Q2_OK);
    CHECK(q2_keys_component(keys, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_data_component(d, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_data_register(d, &owner, &spec, &edge) == Q2_ERR_STATE);
    q2_runtime_driver_t recorder = {1, sizeof(recorder), &owner, event, noop, noop};
    CHECK(q2_runtime_attach(k, &recorder) == Q2_OK);
    q2_caller_t a = {.instance = 10, .grants = {"data.read", "power.read", "input.keys"}, .grant_count = 3};
    q2_caller_t denied = {.instance = 11, .grants = {"data.read"}, .grant_count = 1};
    CHECK(call(k, &denied, "get", "power.battery.percent", &out) == Q2_ERR_PERMISSION);
    CHECK(call(k, &a, "get", "missing", &out) == Q2_ERR_NOT_FOUND);
    CHECK(call(k, &a, "get", "power.battery.percent", &out) == Q2_ERR_STATE);
    CHECK(call(k, &a, "subscribe", "power.battery.percent", &out) == Q2_OK);
    int64_t sub = out.as.integer;
    CHECK(call(k, &a, "subscribe", "power.battery.percent", &out) == Q2_ERR_EXISTS);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 1 && last.data_status == Q2_ERR_STATE);
    q2_value_t sample = {.kind = Q2_INTEGER, .as.integer = 0};
    CHECK(q2_data_publish(d, &other, battery, &sample, Q2_OK) == Q2_ERR_PERMISSION);
    sample.as.integer = 101;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_ERR_ARGUMENT);
    sample.as.integer = 0;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(call(k, &a, "get", "power.battery.percent", &out) == Q2_OK && out.as.integer == 0);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 2 && last.data.as.integer == 0 && last.sequence == 1);
    clock_ms = 50;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 2);
    clock_ms = 149;
    CHECK(call(k, &a, "get", "power.battery.percent", &out) == Q2_OK);
    clock_ms = 150;
    CHECK(call(k, &a, "get", "power.battery.percent", &out) == Q2_ERR_TIMEOUT);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 3 && last.data_status == Q2_ERR_TIMEOUT);
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    sample.as.integer = 3;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 4 && last.data.as.integer == 3);
    sample.as.integer = 4;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    q2_value_t token = {.kind = Q2_INTEGER, .as.integer = sub};
    CHECK(q2_kernel_call(k, &denied, "data", "unsubscribe", &token, 1, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &a, "data", "unsubscribe", &token, 1, &out) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 4);
    CHECK(call(k, &a, "get", "test.edge", &out) == Q2_ERR_UNSUPPORTED);
    CHECK(call(k, &a, "subscribe", "test.edge", &out) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 4);
    CHECK(q2_data_focus(d, a.instance) == Q2_OK);
    sample.as.integer = 1;
    CHECK(q2_data_publish(d, &owner, edge, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_data_publish(d, &owner, edge, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 6);
    CHECK(q2_data_publish(d, &owner, edge, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_data_focus(d, 99) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 6);
    CHECK(q2_data_focus(d, a.instance) == Q2_OK);
    for (unsigned i = 0; i < Q2_EVENT_MAX; i++)
        CHECK(q2_data_publish(d, &owner, edge, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_data_publish(d, &owner, edge, &sample, Q2_OK) == Q2_ERR_LIMIT);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == 38);
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    for (unsigned i = 0; i < 17; i++) {
        char name[32];
        snprintf(name, sizeof(name), "test.state%u", i);
        CHECK(call(k, &a, "subscribe", name, &out) == (i < 16 ? Q2_OK : Q2_ERR_LIMIT));
    }
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    /* The generic key topic cannot reveal raw actions while lifecycle withholds forwarding. */
    CHECK(call(k, &a, "subscribe", "input.key.POWER.action", &out) == Q2_OK);
    CHECK(call(k, &a, "get", "input.key.POWER.action", &out) == Q2_ERR_UNSUPPORTED);
    CHECK(q2_keys_route(keys, 20, a.instance) == Q2_OK);
    unsigned before = received;
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_DOUBLE) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before);
    q2_caller_t router = {.instance = 20, .grants = {"system.lifecycle"}, .grant_count = 1};
    q2_value_t args[] = {text("POWER"), {.kind = Q2_INTEGER, .as.integer = 2}};
    CHECK(q2_kernel_call(k, &router, "keys", "forward", args, 2, &out) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before + 1 && last.data.as.integer == 2);
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    /* A full shared queue must delay, not lose, a cached state notification. */
    CHECK(call(k, &a, "subscribe", "power.battery.percent", &out) == Q2_OK);
    for (unsigned i = 0; i < Q2_EVENT_MAX; i++) {
        q2_event_t filler = {.instance = 500, .topic = "filler"};
        CHECK(q2_event_post(k, &filler) == Q2_OK);
    }
    sample.as.integer = 33;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    before = received;
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before + 1 && last.data.as.integer == 33);
    CHECK(q2_data_publish(d, &owner, battery, NULL, Q2_ERR_IO) == Q2_OK);
    CHECK(call(k, &a, "get", "power.battery.percent", &out) == Q2_ERR_IO);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && !last.data.kind && last.data_status == Q2_ERR_IO);
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    q2_value_t watch_args[] = {text("power.battery.percent"),
                               {.kind = Q2_BOOLEAN, .as.boolean = false},
                               {.kind = Q2_INTEGER, .as.integer = 50}};
    CHECK(q2_kernel_call(k, &a, "data", "subscribe", watch_args, 3, &out) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    sample.as.integer = 40;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    before = received;
    sample.as.integer = 41;
    CHECK(q2_data_publish(d, &owner, battery, &sample, Q2_OK) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before);
    clock_ms += 50;
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && received == before + 1 && last.data.as.integer == 41);
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    q2_value_t title_value = text("Copied title");
    CHECK(q2_data_publish(d, &owner, title, &title_value, Q2_OK) == Q2_OK);
    strcpy(title_value.as.string, "Changed buffer");
    CHECK(call(k, &a, "get", "test.title", &out) == Q2_OK && !strcmp(out.as.string, "Copied title"));
    CHECK(call(k, &a, "subscribe", "test.title", &out) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK && last.data.kind == Q2_STRING &&
          !strcmp(last.data.as.string, "Copied title"));
    CHECK(q2_kernel_release_app(k, a.instance) == Q2_OK);
    /* Run actual Lua bindings: false is a value, notifications carry typed metadata and invalidity. */
    q2_lua_runtime_t *lua = NULL;
    CHECK(q2_lua_create(k, &lua) == Q2_OK);
    const char *manifest =
        "{\"format\":2,\"id\":\"71f147c6-3604-4474-9e3d-af2d949fb38f\",\"name\":\"Data "
        "test\",\"version\":\"1.0.0\",\"type\":\"app\",\"entry\":\"main.lua\",\"api\":2,\"capabilities\":{"
        "\"required\":[\"data.read\",\"power.read\"],\"optional\":[]},\"resources\":{\"lua_heap_kib\":64}}";
    const char *code =
        "local q=require('q2');local sub;return {on_start=function() "
        "local v,e=q.data.get('power.battery.charging');assert(v==false);"
        "sub=assert(q.data.subscribe('power.battery.charging'));end,"
        "on_event=function(e) assert(e.type=='data.changed' and e.key=='power.battery.charging');"
        "assert(e.value==false and e.valid and e.status=='ok' and e.subscription==sub);"
        "assert(q.data.unsubscribe(sub));q.os.exit() end}";
    sample.kind = Q2_BOOLEAN;
    sample.as.boolean = false;
    CHECK(q2_data_publish(d, &owner, charging, &sample, Q2_OK) == Q2_OK);
    const char *grants[] = {"data.read", "power.read"};
    CHECK(q2_lua_start(lua, manifest, strlen(manifest), code, strlen(code), grants, 2) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    q2_app_status_t status;
    CHECK(q2_lua_status(lua, &status) == Q2_OK && !status.running && status.last_result == Q2_OK);
    /* One VM exiting/unsubscribing must not cancel another VM's same-key subscription. */
    q2_lua_runtime_t *second = NULL;
    CHECK(q2_lua_create(k, &second) == Q2_OK);
    const char *other_code =
        "local q=require('q2');local n=0;return {on_start=function() "
        "assert(q.data.subscribe('power.battery.charging')) end,on_event=function(e) n=n+1;"
        "if n==1 then assert(e.valid and e.value==false) else assert(not e.valid and e.value==nil "
        "and e.status=='io_error');q.os.exit() end end}";
    CHECK(q2_lua_start(lua, manifest, strlen(manifest), code, strlen(code), grants, 2) == Q2_OK);
    CHECK(q2_lua_start(second, manifest, strlen(manifest), other_code, strlen(other_code), grants, 2) ==
          Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    CHECK(q2_lua_status(lua, &status) == Q2_OK && !status.running);
    CHECK(q2_lua_status(second, &status) == Q2_OK && status.running);
    CHECK(q2_data_publish(d, &owner, charging, NULL, Q2_ERR_IO) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    CHECK(q2_lua_status(second, &status) == Q2_OK && !status.running && status.last_result == Q2_OK);
    CHECK(q2_lua_destroy(&second) == Q2_OK);
    CHECK(q2_lua_destroy(&lua) == Q2_OK);
    CHECK(q2_runtime_detach(k, &owner) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_keys_destroy(&keys) == Q2_OK);
    CHECK(q2_data_destroy(&d) == Q2_OK);
    printf("PASS: %u data contract checks\n", checks);
    return 0;
}
