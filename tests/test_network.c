#include "q2/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, started, released;
static uint64_t clock_ms;
static q2_result_t transport_result = Q2_ERR_BUSY;
static bool corrupt;
static const char *json_body;
static char posted[Q2_NETWORK_UPLOAD_MAX + 1];
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
static q2_result_t begin(void *c, const char *url, unsigned timeout, void **job) {
    (void)c;
    (void)url;
    (void)timeout;
    *job = malloc(1);
    if (!*job)
        return Q2_ERR_MEMORY;
    started++;
    return Q2_OK;
}
static q2_result_t post_job(void *c, const char *url, const char *body, size_t size, unsigned timeout,
                            void **job) {
    memcpy(posted, body, size);
    posted[size] = 0;
    return begin(c, url, timeout, job);
}
static q2_result_t poll_job(void *c, void *job, q2_network_response_t *out) {
    (void)c;
    (void)job;
    *out = (q2_network_response_t){404, corrupt ? "x\0y" : "hello", corrupt ? 3 : 5};
    if (json_body)
        *out = (q2_network_response_t){200, json_body, strlen(json_body)};
    return transport_result;
}
static q2_result_t close_job(void *c, void **job) {
    (void)c;
    if (*job) {
        released++;
        free(*job);
        *job = NULL;
    }
    return Q2_OK;
}
static q2_value_t str(const char *text) {
    q2_value_t v = {.kind = Q2_STRING};
    strcpy(v.as.string, text);
    return v;
}
static q2_value_t num(int64_t n) {
    q2_value_t v = {.kind = Q2_INTEGER, .as.integer = n};
    return v;
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_kernel_t *k = NULL;
    q2_network_t *net = NULL;
    q2_network_backend_t backend = {2, sizeof(backend), NULL, begin, poll_job, close_job, post_job};
    const q2_component_t *component;
    q2_value_t out, a[3];
    q2_caller_t user = {.instance = 1, .grants = {"network.http"}, .grant_count = 1}, other = user;
    strcpy(user.app_id, "63b11f7d-f51f-44e5-aa58-97e15e77b3d0");
    other.instance = 2;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_network_create(k, &backend, &net) == Q2_OK);
    CHECK(q2_network_component(net, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_network_destroy(&net) == Q2_ERR_STATE);
    a[0] = str("https://example.com/");
    other.grant_count = 0;
    CHECK(q2_kernel_call(k, &other, "network", "get", a, 1, &out) == Q2_ERR_PERMISSION);
    CHECK(started == 0);
    other.grant_count = 1;
    const char *bad[] = {"ftp://x",      "http://",     "https://u:p@host/",
                         "http://x/a b", "http://x/#y", "http://x/\\y"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        a[0] = str(bad[i]);
        CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_ERR_ARGUMENT);
    }
    a[0] = str("https://example.com/");
    a[1] = num(99);
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 2, &out) == Q2_ERR_ARGUMENT);
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_OK);
    int64_t first = out.as.integer;
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_ERR_BUSY);
    a[0] = num(first);
    CHECK(q2_kernel_call(k, &other, "network", "status", a, 1, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &other, "network", "close", a, 1, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_OK &&
          !strcmp(out.as.string, "pending"));
    CHECK(q2_kernel_call(k, &user, "network", "code", a, 1, &out) == Q2_ERR_BUSY);
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    transport_result = Q2_OK;
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_OK &&
          !strcmp(out.as.string, "done"));
    CHECK(q2_kernel_call(k, &user, "network", "code", a, 1, &out) == Q2_OK && out.as.integer == 404);
    a[1] = num(1);
    a[2] = num(3);
    CHECK(q2_kernel_call(k, &user, "network", "read", a, 3, &out) == Q2_OK && !strcmp(out.as.string, "ell"));
    a[1] = num(5);
    CHECK(q2_kernel_call(k, &user, "network", "read", a, 2, &out) == Q2_OK && !*out.as.string);
    a[1] = num(6);
    CHECK(q2_kernel_call(k, &user, "network", "read", a, 2, &out) == Q2_ERR_ARGUMENT);
    a[1] = num(-1);
    CHECK(q2_kernel_call(k, &user, "network", "read", a, 2, &out) == Q2_ERR_ARGUMENT);
    CHECK(q2_kernel_call(k, &user, "network", "close", a, 1, &out) == Q2_OK);
    CHECK(released == 1);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_ERR_NOT_FOUND);
    transport_result = Q2_ERR_BUSY;
    a[0] = str("http://localhost/");
    a[1] = num(100);
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 2, &out) == Q2_OK);
    CHECK(out.as.integer > first);
    a[0] = out;
    clock_ms = 100;
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_ERR_TIMEOUT);
    CHECK(released == 2);
    CHECK(q2_kernel_release_app(k, 1) == Q2_OK);
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++) {
        user.instance = i + 1;
        a[0] = str("http://localhost/");
        CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_OK);
    }
    user.instance = 10;
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_ERR_LIMIT);
    CHECK(q2_kernel_release_app(k, 1) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_OK);
    a[0] = out;
    transport_result = Q2_OK;
    corrupt = true;
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_ERR_FORMAT);
    for (unsigned instance = 1; instance <= 10; instance++)
        CHECK(q2_kernel_release_app(k, instance) == Q2_OK);
    a[0] = str("https://example.com/login");
    CHECK(q2_kernel_call(k, &user, "network", "post", a, 1, &out) == Q2_OK);
    a[0] = out;
    int64_t post_id = out.as.integer;
    a[1] = str("{\"cookie\":\"test-secret\"}");
    CHECK(q2_kernel_call(k, &other, "network", "write", a, 2, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &user, "network", "status", a, 1, &out) == Q2_ERR_STATE);
    CHECK(q2_kernel_call(k, &user, "network", "write", a, 2, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "send", a, 1, &out) == Q2_OK);
    CHECK(!strcmp(posted, "{\"cookie\":\"test-secret\"}"));
    CHECK(q2_kernel_call(k, &user, "network", "send", a, 1, &out) == Q2_ERR_STATE);
    CHECK(q2_kernel_call(k, &user, "network", "write", a, 2, &out) == Q2_ERR_STATE);
    json_body =
        "{\"data\":[{\"uid\":4294967296,\"ok\":false,\"s\":\"abc\",\"empty\":null}],\"a/b\":{\"~\":7}}";
    corrupt = false;
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    a[1] = str("/data");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_OK && out.as.integer == 1);
    a[1] = str("/data/0/uid");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_OK && out.as.integer == 4294967296LL);
    CHECK(q2_kernel_call(k, &other, "network", "json", a, 2, &out) == Q2_ERR_NOT_FOUND);
    a[1] = str("/data/0/ok");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_OK && out.kind == Q2_BOOLEAN &&
          !out.as.boolean);
    a[1] = str("/data/0/empty");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_ERR_NOT_FOUND);
    a[1] = str("/a~1b/~0");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_OK && out.as.integer == 7);
    a[1] = str("/data/0/s");
    a[2] = num(1);
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 3, &out) == Q2_OK && !strcmp(out.as.string, "bc"));
    a[2] = num(4);
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 3, &out) == Q2_ERR_ARGUMENT);
    CHECK(q2_kernel_call(k, &user, "network", "close", a, 1, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_ERR_NOT_FOUND);
    a[0] = str("https://example.com/");
    CHECK(q2_kernel_call(k, &user, "network", "post", a, 1, &out) == Q2_OK && out.as.integer > post_id);
    a[0] = out;
    memset(a[1].as.string, 'x', 255);
    a[1].as.string[255] = 0;
    a[1].kind = Q2_STRING;
    for (unsigned chunk = 0; chunk < 32; chunk++)
        CHECK(q2_kernel_call(k, &user, "network", "write", a, 2, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "write", a, 2, &out) == Q2_ERR_LIMIT);
    CHECK(q2_kernel_release_app(k, user.instance) == Q2_OK);
    a[0] = str("https://example.com/");
    CHECK(q2_kernel_call(k, &user, "network", "get", a, 1, &out) == Q2_OK);
    a[0] = out;
    a[1] = str("/code");
    json_body = "{\"code\":200}garbage";
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_ERR_FORMAT);
    const q2_network_binary_t *binary = NULL;
    CHECK(q2_service_get(k, "network.binary", 1, sizeof(*binary), (const void **)&binary) == Q2_OK);
    uint64_t binary_id = 0;
    other.grant_count = 0;
    CHECK(binary->get(binary->context, &other, "http://localhost/cover", 1000, &binary_id) ==
          Q2_ERR_PERMISSION);
    other.grant_count = 1;
    /* A binary request coexists with the current text request and preserves embedded NULs. */
    CHECK(binary->get(binary->context, &user, "http://localhost/cover", 1000, &binary_id) == Q2_OK);
    q2_network_response_t bytes;
    CHECK(binary->read(binary->context, &other, binary_id, &bytes) == Q2_ERR_NOT_FOUND);
    CHECK(binary->read(binary->context, &user, binary_id, &bytes) == Q2_ERR_BUSY);
    corrupt = true;
    json_body = NULL;
    CHECK(q2_kernel_poll(k, 0) == Q2_OK);
    CHECK(binary->read(binary->context, &user, binary_id, &bytes) == Q2_OK);
    CHECK(bytes.size == 3 && !memcmp(bytes.body, "x\0y", 3));
    a[0] = num((int64_t)binary_id);
    a[1] = num(0);
    CHECK(q2_kernel_call(k, &user, "network", "read", a, 2, &out) == Q2_ERR_UNSUPPORTED);
    a[1] = str("/code");
    CHECK(q2_kernel_call(k, &user, "network", "json", a, 2, &out) == Q2_ERR_UNSUPPORTED);
    CHECK(binary->close(binary->context, &other, binary_id) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_release_app(k, user.instance) == Q2_OK);
    CHECK(binary->read(binary->context, &user, binary_id, &bytes) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(started == released);
    CHECK(q2_network_destroy(&net) == Q2_OK);
    printf("PASS: %u network ownership, quota, deadline, response and permission checks\n", checks);
    return 0;
}
