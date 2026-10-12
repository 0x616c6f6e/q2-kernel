#include "q2/keys.h"
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                  \
            exit(1);                                                                                         \
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
    return 0;
}
static q2_result_t ready(q2_kernel_t *k, void *ctx) {
    (void)k;
    (void)ctx;
    return Q2_OK;
}
static unsigned values[2], tick_counts[2];
static q2_result_t record(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    (void)ctx;
    (void)out;
    if (n != 2 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER)
        return Q2_ERR_ARGUMENT;
    unsigned i = caller->app_id[0] == '1' ? 0 : 1;
    values[i] = (unsigned)a[0].as.integer;
    tick_counts[i] = (unsigned)a[1].as.integer;
    return Q2_OK;
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_kernel_t *k = NULL;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    const q2_method_t method = {"probe", "record", "test.record", record};
    q2_component_t component = {.abi = 1,
                                .size = sizeof(component),
                                .id = "probe",
                                .start = ready,
                                .stop = ready,
                                .methods = &method,
                                .method_count = 1};
    CHECK(q2_component_register(k, &component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    q2_lua_runtime_t *vm[Q2_RUNTIME_MAX] = {0}, *extra = NULL;
    for (unsigned i = 0; i < Q2_RUNTIME_MAX; i++)
        CHECK(q2_lua_create(k, &vm[i]) == Q2_OK);
    CHECK(q2_lua_create(k, &extra) == Q2_ERR_LIMIT && !extra);
    const char *grants[] = {"test.record"};
    for (unsigned i = 0; i < 2; i++) {
        char manifest[800], code[800];
        snprintf(manifest, sizeof(manifest),
                 "{\"format\":2,\"id\":\"%u1111111-1111-4111-8111-111111111111\","
                 "\"name\":\"VM\",\"version\":\"1.0.0\",\"entry\":\"main.lua\",\"type\":\"app\",\"api\":2,"
                 "\"capabilities\":{\"required\":[\"test.record\"],\"optional\":[]},\"resources\":{\"lua_"
                 "heap_kib\":64}}",
                 i + 1);
        snprintf(code, sizeof(code),
                 "local q=require('q2');x=%u;local ticks=0;return {"
                 "on_tick=function(now) ticks=ticks+1;assert(q.probe.record(x,ticks)) end,"
                 "on_event=function(e) if e.value==99 then error('isolated fault') end;x=x+e.value;end}",
                 i * 100);
        CHECK(q2_lua_start(vm[i], manifest, strlen(manifest), code, strlen(code), grants, 1) == Q2_OK);
    }
    q2_app_status_t one, two;
    CHECK(q2_lua_status(vm[0], &one) == Q2_OK && one.running);
    CHECK(q2_lua_status(vm[1], &two) == Q2_OK && two.running && one.instance != two.instance);
    q2_event_t event = {.instance = one.instance, .topic = "test", .value = 3};
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(values[0] == 3 && values[1] == 100 && tick_counts[0] == 1 && tick_counts[1] == 1);
    event.instance = two.instance;
    event.value = 7;
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(values[0] == 3 && values[1] == 107 && tick_counts[0] == 2 && tick_counts[1] == 2);
    event.value = 99;
    CHECK(q2_event_post(k, &event) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_ERR_RUNTIME);
    CHECK(q2_lua_status(vm[1], &two) == Q2_OK && !two.running);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK && tick_counts[0] == 4 && tick_counts[1] == 2);
    CHECK(q2_kernel_destroy(&k) == Q2_ERR_BUSY);
    for (unsigned i = 0; i < Q2_RUNTIME_MAX; i++)
        CHECK(q2_lua_destroy(&vm[i]) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    printf(
        "PASS: %u multi-VM checks; isolation, addressed events, independent ticks, fault isolation, limits\n",
        checks);
    return 0;
}
