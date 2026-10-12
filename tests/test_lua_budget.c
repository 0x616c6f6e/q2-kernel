/* Deterministic elapsed-time regression: CPU bootstrap, provider latency and Lua quotas are separate. */
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr);                                               \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static uint64_t ticks;
static unsigned allocation_ms, clock_step, provider_calls;
static void *allocate(void *ctx, size_t size) {
    (void)ctx;
    ticks += allocation_ms;
    return malloc(size);
}
static void release(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}
static uint64_t now(void *ctx) {
    (void)ctx;
    ticks += clock_step;
    return ticks;
}
static q2_result_t ready(q2_kernel_t *k, void *ctx) {
    (void)k;
    (void)ctx;
    return Q2_OK;
}
static q2_result_t delay(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)ctx;
    (void)c;
    (void)out;
    CHECK(n == 1 && a[0].kind == Q2_INTEGER);
    provider_calls++;
    ticks += 250; /* Simulate bounded display/storage work without sleeping. */
    return a[0].as.integer ? Q2_ERR_IO : Q2_OK;
}
static const char manifest[] =
    "{\"format\":2,\"id\":\"11111111-1111-4111-8111-111111111111\","
    "\"name\":\"Budget\",\"version\":\"1.0.0\",\"type\":\"app\",\"api\":2,"
    "\"entry\":\"main.lua\",\"capabilities\":{\"required\":[\"test.delay\"],\"optional\":[]},"
    "\"resources\":{\"lua_heap_kib\":64}}";
static q2_result_t run(q2_lua_runtime_t *runtime, const char *source) {
    const char *grants[] = {"test.delay"};
    q2_result_t e = q2_lua_start(runtime, manifest, strlen(manifest), source, strlen(source), grants, 1);
    if (e != Q2_OK) {
        q2_app_status_t status;
        CHECK(q2_lua_status(runtime, &status) == Q2_OK);
        fprintf(stderr, "Observed: %s\n", status.error);
    }
    return e;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    q2_kernel_t *kernel = NULL;
    q2_lua_runtime_t *runtime = NULL;
    const q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    const q2_method_t method = {"probe", "delay", "test.delay", delay};
    const q2_component_t component = {.abi = 1,
                                      .size = sizeof(component),
                                      .id = "probe",
                                      .start = ready,
                                      .stop = ready,
                                      .methods = &method,
                                      .method_count = 1};
    CHECK(q2_kernel_create(&port, &kernel) == Q2_OK);
    CHECK(q2_component_register(kernel, &component) == Q2_OK);
    CHECK(q2_kernel_boot(kernel) == Q2_OK);
    CHECK(q2_lua_create(kernel, &runtime) == Q2_OK);
    if (!strcmp(argv[1], "bootstrap")) {
        /* Library setup and compilation take hundreds of fake milliseconds; Lua itself fits. */
        allocation_ms = 1;
        CHECK(run(runtime, "local sum=0;for i=1,3000 do sum=sum+i end;return {}") == Q2_OK);
        CHECK(ticks > 30);
        allocation_ms = 0;
    } else if (!strcmp(argv[1], "native")) {
        const char *source = "local q=require('q2');local function work() "
                             "assert(q.probe.delay(0));local v,e=q.probe.delay(1);assert(v==nil and e.code);"
                             "local sum=0;for i=1,3000 do sum=sum+i end end;"
                             "return {on_start=work,on_event=work,on_tick=work}";
        CHECK(run(runtime, source) == Q2_OK);
        q2_app_status_t status;
        CHECK(q2_lua_status(runtime, &status) == Q2_OK);
        q2_event_t event = {.instance = status.instance, .topic = "test"};
        CHECK(q2_event_post(kernel, &event) == Q2_OK);
        CHECK(q2_kernel_poll(kernel, 16) == Q2_OK);
        CHECK(provider_calls == 6 && ticks >= 1500);
        CHECK(q2_kernel_poll(kernel, 16) == Q2_OK && provider_calls == 8);
    } else {
        bool wall = !strcmp(argv[1], "wall");
        bool mixed = !strcmp(argv[1], "mixed");
        bool calls = !strcmp(argv[1], "calls");
        clock_step = wall ? 10 : mixed ? 5 : 0;
        CHECK(run(runtime, calls || mixed ? "local q=require('q2');while true do q.probe.delay(0) end"
                                          : "while true do end") == Q2_ERR_RUNTIME);
        q2_app_status_t status;
        CHECK(q2_lua_status(runtime, &status) == Q2_OK);
        CHECK(!status.running && status.heap_bytes == 0);
        CHECK(strstr(status.error, wall || mixed ? "time" : "instructions"));
        if (mixed)
            CHECK(provider_calls > 0 && provider_calls < 10);
        if (calls)
            CHECK(provider_calls > 0 && provider_calls < 30000);
        /* A failed callback cannot poison the next VM instance's budget. */
        clock_step = 0;
        CHECK(run(runtime, "return {}") == Q2_OK);
    }
    CHECK(q2_lua_destroy(&runtime) == Q2_OK);
    CHECK(q2_kernel_destroy(&kernel) == Q2_OK);
    printf("PASS: Lua budget %s\n", argv[1]);
    return 0;
}
