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
static const char manifest[] =
    "{\"format\":2,\"id\":\"11111111-1111-4111-8111-111111111111\",\"name\":\"Keys\",\"version\":\"1.0.0\","
    "\"type\":\"app\",\"entry\":\"main.lua\",\"api\":2,\"capabilities\":{\"required\":[\"input.keys\"],"
    "\"optional\":[]},\"resources\":{\"lua_heap_kib\":64}}";
static const char *grants[] = {"input.keys"};
static q2_result_t run(q2_lua_runtime_t *r, const char *code) {
    return q2_lua_start(r, manifest, strlen(manifest), code, strlen(code), grants, 1);
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_kernel_t *k = NULL;
    q2_keys_t *keys = NULL;
    q2_lua_runtime_t *r = NULL;
    const q2_component_t *component;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_keys_create(k, &keys) == Q2_OK);
    CHECK(q2_keys_register(keys, "POWER") == Q2_OK);
    CHECK(q2_keys_register(keys, "POWER") == Q2_ERR_EXISTS);
    CHECK(q2_keys_register(keys, "power.alt-1") == Q2_OK);
    CHECK(q2_keys_register(keys, "1bad") == Q2_ERR_ARGUMENT);
    CHECK(q2_keys_register(keys, "a/b") == Q2_ERR_ARGUMENT);
    CHECK(q2_keys_register(keys, "") == Q2_ERR_ARGUMENT);
    CHECK(q2_keys_register(keys, "abcdefghijklmnopqrstuvwxyz123456") == Q2_ERR_ARGUMENT);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_SINGLE) == Q2_ERR_STATE);
    CHECK(q2_keys_component(keys, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_keys_register(keys, "LATE") == Q2_ERR_STATE);
    CHECK(q2_keys_destroy(&keys) == Q2_ERR_BUSY);
    CHECK(q2_keys_report(keys, "missing", Q2_KEY_SINGLE) == Q2_ERR_NOT_FOUND);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_NONE) == Q2_ERR_ARGUMENT);
    CHECK(q2_keys_report(keys, "POWER", (q2_key_action_t)4) == Q2_ERR_ARGUMENT);
    q2_key_state_t snapshot;
    CHECK(q2_keys_at(keys, 0, &snapshot) == Q2_OK && snapshot.last_action == Q2_KEY_NONE);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_LONG) == Q2_OK);
    q2_kernel_status_t ks;
    CHECK(q2_kernel_status(k, &ks) == Q2_OK && ks.queued == 0);
    CHECK(q2_keys_at(keys, 0, &snapshot) == Q2_OK && snapshot.last_action == Q2_KEY_LONG);
    q2_caller_t denied = {.instance = 100};
    q2_value_t value, arg = {.kind = Q2_BOOLEAN, .as.boolean = true};
    CHECK(q2_kernel_call(k, &denied, "keys", "watch", &arg, 1, &value) == Q2_ERR_PERMISSION);
    CHECK(q2_lua_create(k, &r) == Q2_OK);
    CHECK(run(r, "local q=require('q2');local i=0;return {on_start=function() "
                 "assert(q.keys.count()==2);assert(q.keys.name(1)=='POWER');assert(q.keys.last('POWER')==3);"
                 "assert(q.keys.watch(true)) end,on_event=function(e) local "
                 "actions={1,1,2,3};i=i+1;assert(e.type=='key.action' and e.text=='POWER' and "
                 "e.value==actions[i]);if i==4 then q.os.exit() end end}") == Q2_OK);
    const q2_key_action_t actions[] = {Q2_KEY_SINGLE, Q2_KEY_SINGLE, Q2_KEY_DOUBLE, Q2_KEY_LONG};
    for (unsigned i = 0; i < 4; i++)
        CHECK(q2_keys_report(keys, "POWER", actions[i]) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    q2_app_status_t status;
    CHECK(q2_lua_status(r, &status) == Q2_OK && !status.running);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_DOUBLE) == Q2_OK);
    CHECK(q2_kernel_status(k, &ks) == Q2_OK && ks.queued == 0);
    CHECK(run(r, "local q=require('q2');assert(q.keys.watch(true));return {}") == Q2_OK);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_LONG) == Q2_OK);
    CHECK(q2_lua_stop(r) == Q2_OK);
    CHECK(run(r, "local q=require('q2');assert(q.keys.watch(true));return {on_event=function(e) "
                 "assert(e.value==1);q.os.exit() end}") == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(q2_lua_status(r, &status) == Q2_OK && status.running);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_SINGLE) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(q2_lua_status(r, &status) == Q2_OK && !status.running);
    CHECK(run(r, "local q=require('q2');assert(q.keys.watch(true));return {}") == Q2_OK);
    for (unsigned i = 0; i < Q2_EVENT_MAX; i++)
        CHECK(q2_keys_report(keys, "POWER", Q2_KEY_SINGLE) == Q2_OK);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_LONG) == Q2_ERR_LIMIT);
    CHECK(q2_keys_at(keys, 0, &snapshot) == Q2_OK && snapshot.last_action == Q2_KEY_LONG);
    CHECK(q2_kernel_status(k, &ks) == Q2_OK && ks.dropped == 1);
    CHECK(q2_lua_stop(r) == Q2_OK);
    CHECK(q2_kernel_poll(k, 32) == Q2_OK);
    CHECK(run(r, "local q=require('q2');assert(q.keys.watch(true));assert(q.keys.watch(false));return "
                 "{on_event=function() error('unsubscribed') end}") == Q2_OK);
    CHECK(q2_keys_report(keys, "POWER", Q2_KEY_DOUBLE) == Q2_OK);
    CHECK(q2_kernel_poll(k, 16) == Q2_OK);
    CHECK(q2_kernel_status(k, &ks) == Q2_OK && ks.queued == 0);
    CHECK(q2_lua_destroy(&r) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_keys_destroy(&keys) == Q2_OK);
    printf("PASS: %u named key contracts\n", checks);
    return 0;
}
