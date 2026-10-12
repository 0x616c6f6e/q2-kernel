#include "q2/apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, preparations;
static bool fail_release, fail_prepare;
static char prepared[37];
#define HOME "11111111-1111-4111-8111-111111111111"
#define OTHER "22222222-2222-4222-8222-222222222222"
#define EXIT "33333333-3333-4333-8333-333333333333"
#define BAD "44444444-4444-4444-8444-444444444444"
#define DENIED "55555555-5555-4555-8555-555555555555"
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
static q2_result_t prepare(void *c, const char *id) {
    (void)c;
    preparations++;
    strcpy(prepared, id);
    return fail_prepare && !strcmp(id, OTHER) ? Q2_ERR_IO : Q2_OK;
}
static bool fail_poll;
static q2_result_t provider_poll(q2_kernel_t *k, void *c) {
    (void)k;
    (void)c;
    return fail_poll ? Q2_ERR_RUNTIME : Q2_OK;
}
static q2_result_t lifecycle(q2_kernel_t *k, void *c) {
    (void)k;
    (void)c;
    return Q2_OK;
}
static q2_result_t release_app(q2_kernel_t *k, void *c, uint64_t id) {
    (void)k;
    (void)c;
    (void)id;
    return fail_release ? Q2_ERR_IO : Q2_OK;
}
static q2_result_t add(q2_apps_t *apps, const char *id, const char *code, bool factory, bool home,
                       bool granted) {
    char manifest[1024];
    snprintf(
        manifest, sizeof(manifest),
        "{\"format\":2,\"id\":\"%s\",\"name\":\"Test\",\"version\":\"1.0.0\","
        "\"type\":\"app\",\"api\":2,\"entry\":\"main.lua\",\"capabilities\":{\"required\":[\"apps.list\"],"
        "\"optional\":[\"apps.navigate\",\"apps.launch\",\"appearance.write\",\"appearance.read\"]},"
        "\"resources\":{\"lua_heap_kib\":64}}",
        id);
    const char *grants[] = {"apps.list", "apps.navigate", "apps.launch", "appearance.write",
                            "appearance.read"};
    q2_app_package_t p = {.manifest = manifest,
                          .source = code,
                          .manifest_size = strlen(manifest),
                          .source_size = strlen(code),
                          .grants = grants,
                          .grant_count = granted ? 5 : 0,
                          .factory = factory,
                          .home = home};
    return q2_apps_register(apps, &p);
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_kernel_t *k = NULL;
    q2_lua_runtime_t *r = NULL;
    q2_apps_t *m = NULL;
    const q2_component_t *component;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_apps_create(k, prepare, NULL, &m) == Q2_OK);
    CHECK(q2_apps_request(m, NULL) == Q2_ERR_STATE);
    CHECK(add(m, HOME, "return {}", false, true, true) == Q2_ERR_ARGUMENT);
    CHECK(add(m, HOME, "return {}", true, true, true) == Q2_OK);
    CHECK(add(m, HOME, "return {}", true, true, true) == Q2_ERR_EXISTS);
    CHECK(add(m, OTHER, "return {}", true, true, true) == Q2_ERR_EXISTS);
    CHECK(add(m, OTHER, "return {}", false, false, true) == Q2_OK);
    CHECK(add(m, EXIT, "local q=require('q2');return {on_start=function() q.os.exit() end}", false, false,
              true) == Q2_OK);
    CHECK(add(m, BAD, "error('bad target')", false, false, true) == Q2_OK);
    CHECK(add(m, DENIED, "return {}", false, false, false) == Q2_OK);
    CHECK(q2_apps_component(m, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    q2_component_t guard = {.abi = 1,
                            .size = sizeof(guard),
                            .id = "guard",
                            .required = true,
                            .start = lifecycle,
                            .stop = lifecycle,
                            .release_app = release_app,
                            .poll = provider_poll};
    CHECK(q2_component_register(k, &guard) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(add(m, HOME, "return {}", true, true, true) == Q2_ERR_STATE);
    CHECK(q2_lua_create(k, &r) == Q2_OK);
    CHECK(q2_apps_bind(m, r) == Q2_OK);
    CHECK(q2_apps_bind(m, r) == Q2_ERR_STATE);
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_OK && !strcmp(prepared, HOME));
    q2_app_status_t before, after;
    CHECK(q2_lua_status(r, &before) == Q2_OK && before.running);
    CHECK(q2_apps_request(m, DENIED) == Q2_ERR_PERMISSION);
    CHECK(q2_apps_request(m, "missing") == Q2_ERR_NOT_FOUND);
    CHECK(q2_apps_request(m, OTHER) == Q2_OK);
    CHECK(q2_apps_request(m, NULL) == Q2_ERR_BUSY);
    CHECK(q2_lua_status(r, &after) == Q2_OK && after.instance == before.instance);
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_OK && !strcmp(prepared, OTHER));
    CHECK(q2_lua_status(r, &after) == Q2_OK && after.instance == before.instance);
    uint64_t resident, foreground;
    unsigned running;
    CHECK(q2_apps_state(m, &resident, &foreground, &running) == Q2_OK);
    CHECK(resident == before.instance && foreground != resident && running == 2);
    CHECK(q2_apps_advance(m, Q2_ERR_IO) == Q2_ERR_IO);
    CHECK(q2_apps_request(m, EXIT) == Q2_OK);
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_OK);
    CHECK(q2_apps_advance(m, q2_kernel_poll(k, 16)) == Q2_OK);
    CHECK(q2_apps_state(m, &resident, &foreground, &running) == Q2_OK && foreground == resident &&
          running == 2);
    CHECK(q2_apps_request(m, BAD) == Q2_OK);
    unsigned n = preparations;
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_OK && preparations == n + 1);
    CHECK(q2_apps_state(m, &resident, &foreground, &running) == Q2_OK && foreground == resident);
    /* Same code as the failed APP must not hide a subsequent provider fault. */
    fail_poll = true;
    CHECK(q2_apps_advance(m, q2_kernel_poll(k, 16)) == Q2_ERR_RUNTIME);
    fail_poll = false;
    CHECK(q2_apps_advance(m, q2_kernel_poll(k, 16)) == Q2_OK);
    /* Returning to a running package does not reload its assets or replace its VM. */
    fail_prepare = true;
    CHECK(q2_apps_request(m, OTHER) == Q2_OK);
    n = preparations;
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_OK && preparations == n);
    fail_prepare = false;
    fail_release = true;
    CHECK(q2_apps_shutdown(m) == Q2_ERR_IO);
    fail_release = false;
    CHECK(q2_apps_shutdown(m) == Q2_OK);
    CHECK(q2_lua_destroy(&r) == Q2_OK);
    CHECK(q2_apps_destroy(&m) == Q2_ERR_BUSY);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_apps_destroy(&m) == Q2_OK);
    /* Broken Home fails exactly once; no hidden restart loop. */
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_apps_create(k, prepare, NULL, &m) == Q2_OK);
    CHECK(add(m, HOME, "error('broken Home')", true, true, true) == Q2_OK);
    CHECK(q2_apps_component(m, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_lua_create(k, &r) == Q2_OK);
    CHECK(q2_apps_bind(m, r) == Q2_OK);
    n = preparations;
    CHECK(q2_apps_advance(m, Q2_OK) == Q2_ERR_RUNTIME && preparations == n + 1);
    CHECK(q2_lua_destroy(&r) == Q2_OK);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(add(m, OTHER, "return {}", false, false, true) == Q2_ERR_STATE);
    CHECK(q2_apps_destroy(&m) == Q2_OK);
    printf("PASS: %u application manager checks\n", checks);
    return 0;
}
