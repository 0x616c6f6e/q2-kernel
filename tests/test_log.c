#include "q2/log.h"
#include "q2/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, records, allocations;
static bool recurse, fail_sink;
static q2_kernel_t *active;
static q2_log_record_t last, last_app;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                     \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void *allocate(void *ctx, size_t size) {
    (void)ctx;
    void *p = malloc(size);
    if (p)
        allocations++;
    return p;
}
static void release(void *ctx, void *p) {
    (void)ctx;
    if (p)
        allocations--;
    free(p);
}
static uint64_t now(void *ctx) {
    (void)ctx;
    return 1234;
}
static q2_result_t write_record(void *ctx, const q2_log_record_t *record) {
    (void)ctx;
    records++;
    last = *record;
    if (!strcmp(record->tag, "lua.app"))
        last_app = *record;
    if (recurse) {
        CHECK(q2_log_write(active, Q2_LOG_ERROR, "nested", "no") == Q2_ERR_BUSY);
        CHECK(q2_log_configure(active, NULL, Q2_LOG_OFF) == Q2_ERR_BUSY);
        CHECK(q2_log_set_level(active, Q2_LOG_OFF) == Q2_ERR_BUSY);
        q2_kernel_t *alias = active;
        CHECK(q2_kernel_destroy(&alias) == Q2_ERR_BUSY && alias == active);
    }
    return fail_sink ? Q2_ERR_IO : Q2_OK;
}
static const char manifest[] =
    "{\"format\":2,\"api\":2,\"id\":\"c72f5be1-6e94-447e-a243-e942093e48ae\","
    "\"name\":\"Log test\",\"version\":\"0.1.0\",\"type\":\"app\",\"entry\":\"main.lua\","
    "\"capabilities\":{\"required\":[],\"optional\":[\"system.log\"]},"
    "\"resources\":{\"lua_heap_kib\":64}}";
static void lua_contracts(q2_kernel_t *k) {
    q2_lua_runtime_t *r = NULL;
    CHECK(q2_lua_create(k, &r) == Q2_OK);
    const char denied[] = "local q=require('q2'); local v,e=q.os.log('info','hidden');"
                          "assert(v==nil and e.number==-9); return {}";
    CHECK(q2_lua_start(r, manifest, strlen(manifest), denied, strlen(denied), NULL, 0) == Q2_OK);
    CHECK(q2_lua_stop(r) == Q2_OK);
    const char *grants[] = {"system.log"};
#if Q2_ENABLE_LOG
    const char source[] = "local q=require('q2'); assert(q.os.log('info','literal %s %n'));"
                          "local a,e=q.os.log('off','invalid'); assert(a==nil and e.number==-1);"
                          "a,e=q.os.log('info',123); assert(a==nil and e.number==-1); return {}";
    CHECK(q2_log_set_level(k, Q2_LOG_INFO) == Q2_OK);
    CHECK(q2_lua_start(r, manifest, strlen(manifest), source, strlen(source), grants, 1) == Q2_OK);
    CHECK(!strcmp(last_app.tag, "lua.app") && strstr(last_app.message, "literal %s %n"));
#else
    /* Explicit host grants cannot create a capability omitted from the binary. */
    CHECK(q2_lua_start(r, manifest, strlen(manifest), denied, strlen(denied), grants, 1) == Q2_OK);
#endif
    CHECK(q2_lua_destroy(&r) == Q2_OK && r == NULL);
}
int main(void) {
    const q2_port_t port = {Q2_PORT_ABI, sizeof(port), NULL, allocate, release, now};
    const q2_log_sink_t sink = {Q2_LOG_ABI, sizeof(sink), NULL, write_record};
    CHECK(q2_kernel_create(&port, &active) == Q2_OK);
    q2_log_level_t level;
    bool enabled = true;
    unsigned side_effect = 0;
    CHECK(q2_log_get_level(active, &level) == Q2_OK && level == Q2_LOG_OFF);
    CHECK(q2_log_enabled(active, Q2_LOG_ERROR, &enabled) == Q2_OK && !enabled);
    Q2_LOGE(active, "test", "%u", ++side_effect);
    CHECK(side_effect == 0 && records == 0);
    CHECK(q2_log_set_level(active, (q2_log_level_t)99) == Q2_ERR_ARGUMENT);
    CHECK(q2_log_write(active, Q2_LOG_OFF, "test", "invalid") == Q2_ERR_ARGUMENT);
#if Q2_ENABLE_LOG
    CHECK(q2_log_configure(active, &sink, Q2_LOG_INFO) == Q2_OK);
    Q2_LOGD(active, "test", "%u", ++side_effect);
    CHECK(side_effect == 0 && records == 0);
    CHECK(q2_log_write(active, Q2_LOG_INFO, "test", "number %d", 42) == Q2_OK);
    CHECK(records == 1 && !strcmp(last.message, "number 42") && !last.truncated);
    CHECK(last.timestamp_ms == 1234 && last.level == Q2_LOG_INFO && !strcmp(last.tag, "test"));
    for (int threshold = Q2_LOG_OFF; threshold <= Q2_LOG_DEBUG; threshold++) {
        CHECK(q2_log_set_level(active, (q2_log_level_t)threshold) == Q2_OK);
        unsigned before = records;
        Q2_LOGE(active, "test", "error");
        Q2_LOGW(active, "test", "warn");
        Q2_LOGI(active, "test", "info");
        Q2_LOGD(active, "test", "debug");
        CHECK(records - before == (unsigned)threshold);
    }
    char long_message[512];
    memset(long_message, 'a', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = 0;
    CHECK(q2_log_write(active, Q2_LOG_INFO, "test", "%s", long_message) == Q2_OK);
    CHECK(last.truncated && strlen(last.message) == Q2_LOG_MESSAGE_MAX - 1);
    recurse = true;
    CHECK(q2_log_write(active, Q2_LOG_ERROR, "test", "recursive sink") == Q2_OK);
    recurse = false;
    fail_sink = true;
    CHECK(q2_log_write(active, Q2_LOG_ERROR, "test", "sink failure") == Q2_ERR_IO);
    /* A diagnostic transport failure cannot turn successful boot into a business failure. */
    CHECK(q2_kernel_boot(active) == Q2_OK);
    fail_sink = false;
    CHECK(q2_log_write(active, Q2_LOG_ERROR, "test", "recovered") == Q2_OK);
    q2_kernel_t *other = NULL;
    CHECK(q2_kernel_create(&port, &other) == Q2_OK);
    unsigned before = records;
    Q2_LOGE(other, "test", "independent kernel stays silent");
    CHECK(records == before);
    CHECK(q2_kernel_destroy(&other) == Q2_OK);
    CHECK(q2_log_configure(active, NULL, Q2_LOG_DEBUG) == Q2_OK);
    Q2_LOGE(active, "test", "%u", ++side_effect);
    CHECK(records == before && side_effect == 0);
    CHECK(q2_log_configure(active, &sink, Q2_LOG_INFO) == Q2_OK);
#else
    CHECK(q2_log_configure(active, &sink, Q2_LOG_INFO) == Q2_ERR_UNSUPPORTED);
    CHECK(q2_log_set_level(active, Q2_LOG_INFO) == Q2_ERR_UNSUPPORTED);
    CHECK(q2_log_set_level(active, Q2_LOG_OFF) == Q2_OK);
    CHECK(q2_log_write(active, Q2_LOG_INFO, "test", "disabled") == Q2_ERR_UNSUPPORTED);
    Q2_LOGI(active, "test", "%u", ++side_effect);
    CHECK(side_effect == 0 && records == 0);
    CHECK(q2_kernel_boot(active) == Q2_OK);
#endif
    lua_contracts(active);
    CHECK(q2_kernel_destroy(&active) == Q2_OK && active == NULL);
    CHECK(allocations == 0);
    printf("PASS: %u logging checks (Q2_ENABLE_LOG=%d)\n", checks, Q2_ENABLE_LOG);
    return 0;
}
