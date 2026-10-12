#include "q2/network.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    uint64_t owner, id, deadline;
    void *job;
    char url[Q2_TEXT_MAX], upload[Q2_NETWORK_UPLOAD_MAX + 1];
    size_t upload_size;
    unsigned timeout;
    bool draft, binary;
    cJSON *json;
    q2_result_t result;
    q2_network_response_t response;
} request_t;
struct q2_network {
    q2_kernel_t *kernel;
    q2_network_backend_t backend;
    q2_component_t component;
    request_t requests[Q2_NETWORK_MAX];
    uint64_t next_id;
    bool ready;
    q2_network_binary_t binary;
    q2_service_t service;
};
static q2_result_t clear(q2_network_t *s, request_t *r) {
    q2_result_t e = s->backend.release(s->backend.context, &r->job);
    if (e == Q2_OK) {
        cJSON_Delete(r->json);
        memset(r, 0, sizeof(*r));
    }
    return e;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    (void)k;
    q2_network_t *s = ctx;
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++) {
        request_t *r = &s->requests[i];
        if (r->owner && (!instance || r->owner == instance)) {
            q2_result_t e = clear(s, r);
            if (e != Q2_OK)
                return e;
        }
    }
    return Q2_OK;
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    ((q2_network_t *)ctx)->ready = true;
    return Q2_OK;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    q2_result_t e = release_app(k, ctx, 0);
    if (e == Q2_OK)
        ((q2_network_t *)ctx)->ready = false;
    return e;
}
static q2_result_t poll(q2_kernel_t *k, void *ctx) {
    q2_network_t *s = ctx;
    uint64_t now = 0;
    q2_result_t e = q2_kernel_time(k, &now);
    if (e != Q2_OK)
        return e;
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++) {
        request_t *r = &s->requests[i];
        if (!r->owner || r->result != Q2_ERR_BUSY)
            continue;
        // Deadline belongs to owner: stalled transport never stalls UI or APP cancellation.
        if (now >= r->deadline) {
            e = s->backend.release(s->backend.context, &r->job);
            if (e != Q2_OK)
                return e;
            r->result = Q2_ERR_TIMEOUT;
            continue;
        }
        r->result = s->backend.poll(s->backend.context, r->job, &r->response);
        if (r->result == Q2_OK &&
            (r->response.size > Q2_NETWORK_BODY_MAX || r->response.code < 100 || r->response.code > 599 ||
             (r->response.size &&
              (!r->response.body || (!r->binary && memchr(r->response.body, 0, r->response.size))))))
            r->result = Q2_ERR_FORMAT;
    }
    return Q2_OK; // Per-request failures are inspected by their owning APP, not fatal kernel failures.
}
static bool valid_url(const char *url) {
    const char *host = NULL;
    if (!strncmp(url, "https://", 8))
        host = url + 8;
    else if (!strncmp(url, "http://", 7))
        host = url + 7;
    if (!host || !*host || *host == '/' || *host == '?' || *host == '#')
        return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#')
            return false;
    size_t authority = strcspn(host, "/?");
    return !memchr(host, '@', authority);
}
static q2_result_t get_request(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out, bool binary) {
    q2_network_t *s = ctx;
    if (!c->instance || n < 1 || n > 2 || a[0].kind != Q2_STRING || !valid_url(a[0].as.string) ||
        (n == 2 && (a[1].kind != Q2_INTEGER || a[1].as.integer < 100 || a[1].as.integer > 10000)))
        return Q2_ERR_ARGUMENT;
    request_t *r = NULL;
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++) {
        if (s->requests[i].owner == c->instance && s->requests[i].binary == binary)
            return Q2_ERR_BUSY;
        if (!s->requests[i].owner)
            r = &s->requests[i];
    }
    if (!r || s->next_id == INT64_MAX)
        return Q2_ERR_LIMIT;
    unsigned timeout = n == 2 ? (unsigned)a[1].as.integer : 5000;
    uint64_t now;
    q2_result_t e = q2_kernel_time(k, &now);
    if (e != Q2_OK)
        return e;
    e = s->backend.start(s->backend.context, a[0].as.string, timeout, &r->job);
    if (e != Q2_OK)
        return e;
    r->binary = binary;
    r->owner = c->instance;
    r->id = ++s->next_id;
    r->deadline = now + timeout;
    r->result = Q2_ERR_BUSY;
    out->kind = Q2_INTEGER;
    out->as.integer = (int64_t)r->id;
    return Q2_OK;
}
static q2_result_t get(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                       q2_value_t *out) {
    return get_request(k, ctx, c, a, n, out, false);
}
static request_t *find(q2_network_t *s, const q2_caller_t *c, const q2_value_t *a, size_t n) {
    if (!n || a[0].kind != Q2_INTEGER || a[0].as.integer <= 0)
        return NULL;
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++)
        if (s->requests[i].owner == c->instance && s->requests[i].id == (uint64_t)a[0].as.integer)
            return &s->requests[i];
    return NULL;
}
static q2_result_t status(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    if (n != 1)
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (r->result != Q2_OK && r->result != Q2_ERR_BUSY)
        return r->result;
    out->kind = Q2_STRING;
    strcpy(out->as.string, r->result == Q2_ERR_BUSY ? "pending" : "done");
    return Q2_OK;
}
static q2_result_t code(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    if (n != 1)
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (r->result != Q2_OK)
        return r->result;
    out->kind = Q2_INTEGER;
    out->as.integer = r->response.code;
    return Q2_OK;
}
static q2_result_t read_body(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)k;
    if (n < 2 || n > 3 || a[1].kind != Q2_INTEGER || a[1].as.integer < 0 ||
        (n == 3 && (a[2].kind != Q2_INTEGER || a[2].as.integer < 1 || a[2].as.integer >= Q2_TEXT_MAX)))
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (r->binary)
        return Q2_ERR_UNSUPPORTED;
    if (r->result != Q2_OK)
        return r->result;
    if ((uint64_t)a[1].as.integer > r->response.size)
        return Q2_ERR_ARGUMENT;
    size_t offset = (size_t)a[1].as.integer, length = r->response.size - offset;
    size_t limit = n == 3 ? (size_t)a[2].as.integer : Q2_TEXT_MAX - 1;
    if (length > limit)
        length = limit;
    out->kind = Q2_STRING;
    if (length)
        memcpy(out->as.string, r->response.body + offset, length);
    out->as.string[length] = 0;
    return Q2_OK;
}
static q2_result_t close_request(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a,
                                 size_t n, q2_value_t *out) {
    (void)k;
    (void)out;
    if (n != 1)
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    return clear(ctx, r);
}
/* POST is built in bounded chunks so the generic 255-byte Lua value limit stays unchanged. */
static q2_result_t post(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    q2_network_t *s = ctx;
    if (!c->instance || n < 1 || n > 2 || a[0].kind != Q2_STRING || !valid_url(a[0].as.string) ||
        (n == 2 && (a[1].kind != Q2_INTEGER || a[1].as.integer < 100 || a[1].as.integer > 10000)))
        return Q2_ERR_ARGUMENT;
    if (!s->backend.post)
        return Q2_ERR_UNSUPPORTED;
    request_t *r = NULL;
    for (unsigned i = 0; i < Q2_NETWORK_MAX; i++) {
        if (s->requests[i].owner == c->instance && !s->requests[i].binary)
            return Q2_ERR_BUSY;
        if (!s->requests[i].owner)
            r = &s->requests[i];
    }
    if (!r || s->next_id == INT64_MAX)
        return Q2_ERR_LIMIT;
    r->owner = c->instance;
    r->id = ++s->next_id;
    r->draft = true;
    r->result = Q2_ERR_STATE;
    r->timeout = n == 2 ? (unsigned)a[1].as.integer : 5000;
    strcpy(r->url, a[0].as.string);
    out->kind = Q2_INTEGER;
    out->as.integer = (int64_t)r->id;
    return Q2_OK;
}
static q2_result_t write_body(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)out;
    if (n != 2 || a[1].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (!r->draft)
        return Q2_ERR_STATE;
    size_t length = strlen(a[1].as.string);
    if (length > Q2_NETWORK_UPLOAD_MAX - r->upload_size)
        return Q2_ERR_LIMIT;
    memcpy(r->upload + r->upload_size, a[1].as.string, length + 1);
    r->upload_size += length;
    return Q2_OK;
}
static q2_result_t send_body(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out) {
    (void)out;
    if (n != 1)
        return Q2_ERR_ARGUMENT;
    q2_network_t *s = ctx;
    request_t *r = find(s, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (!r->draft)
        return Q2_ERR_STATE;
    uint64_t now;
    q2_result_t e = q2_kernel_time(k, &now);
    if (e == Q2_OK)
        e = s->backend.post(s->backend.context, r->url, r->upload, r->upload_size, r->timeout, &r->job);
    if (e != Q2_OK)
        return e;
    /* Transport owns its copy, so credentials need not remain in the owner-side staging buffer. */
    memset(r->upload, 0, sizeof(r->upload));
    r->upload_size = 0;
    r->draft = false;
    r->deadline = now + r->timeout;
    r->result = Q2_ERR_BUSY;
    return Q2_OK;
}
/* RFC 6901 pointer traversal. Arrays use zero-based indices; no automatic case folding. */
static cJSON *lookup(cJSON *node, const char *pointer) {
    if (!*pointer)
        return node;
    while (*pointer) {
        if (*pointer++ != '/')
            return NULL;
        char key[Q2_TEXT_MAX];
        size_t length = 0;
        while (*pointer && *pointer != '/') {
            char ch = *pointer++;
            if (ch == '~') {
                if (*pointer == '0')
                    ch = '~';
                else if (*pointer == '1')
                    ch = '/';
                else
                    return NULL;
                pointer++;
            }
            key[length++] = ch;
        }
        key[length] = 0;
        if (cJSON_IsObject(node))
            node = cJSON_GetObjectItemCaseSensitive(node, key);
        else if (cJSON_IsArray(node)) {
            if (!length || (length > 1 && key[0] == '0'))
                return NULL;
            unsigned index = 0;
            for (size_t i = 0; i < length; i++) {
                if (key[i] < '0' || key[i] > '9' || index > 100000)
                    return NULL;
                index = index * 10 + (unsigned)(key[i] - '0');
            }
            node = cJSON_GetArrayItem(node, (int)index);
        } else
            return NULL;
        if (!node)
            return NULL;
    }
    return node;
}
static q2_result_t json_value(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    if (n < 2 || n > 3 || a[1].kind != Q2_STRING ||
        (n == 3 && (a[2].kind != Q2_INTEGER || a[2].as.integer < 0)))
        return Q2_ERR_ARGUMENT;
    request_t *r = find(ctx, c, a, n);
    if (!r)
        return Q2_ERR_NOT_FOUND;
    if (r->binary)
        return Q2_ERR_UNSUPPORTED;
    if (r->result != Q2_OK)
        return r->result;
    if (!r->json) {
        const char *end = NULL;
        r->json = cJSON_ParseWithLengthOpts(r->response.body, r->response.size, &end, false);
        if (!r->json)
            return Q2_ERR_FORMAT;
        while (end < r->response.body + r->response.size &&
               (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
            end++;
        if (end != r->response.body + r->response.size) {
            cJSON_Delete(r->json);
            r->json = NULL;
            return Q2_ERR_FORMAT;
        }
    }
    cJSON *node = lookup(r->json, a[1].as.string);
    if (!node || cJSON_IsNull(node))
        return Q2_ERR_NOT_FOUND;
    if (cJSON_IsString(node)) {
        size_t length = strlen(node->valuestring);
        uint64_t requested = n == 3 ? (uint64_t)a[2].as.integer : 0;
        if (requested > length)
            return Q2_ERR_ARGUMENT;
        size_t offset = (size_t)requested;
        length -= offset;
        if (length >= Q2_TEXT_MAX)
            length = Q2_TEXT_MAX - 1;
        out->kind = Q2_STRING;
        memcpy(out->as.string, node->valuestring + offset, length);
        out->as.string[length] = 0;
    } else if (n == 3)
        return Q2_ERR_ARGUMENT;
    else if (cJSON_IsBool(node)) {
        out->kind = Q2_BOOLEAN;
        out->as.boolean = cJSON_IsTrue(node);
    } else if (cJSON_IsNumber(node) && isfinite(node->valuedouble) &&
               node->valuedouble >= -9007199254740991.0 && node->valuedouble <= 9007199254740991.0 &&
               floor(node->valuedouble) == node->valuedouble) {
        out->kind = Q2_INTEGER;
        out->as.integer = (int64_t)node->valuedouble;
    } else if (cJSON_IsArray(node)) {
        out->kind = Q2_INTEGER;
        out->as.integer = cJSON_GetArraySize(node);
    } else
        return Q2_ERR_UNSUPPORTED;
    return Q2_OK;
}
static const q2_method_t methods[] = {{"network", "get", "network.http", get},
                                      {"network", "post", "network.http", post},
                                      {"network", "write", "network.http", write_body},
                                      {"network", "send", "network.http", send_body},
                                      {"network", "json", "network.http", json_value},
                                      {"network", "status", "network.http", status},
                                      {"network", "code", "network.http", code},
                                      {"network", "read", "network.http", read_body},
                                      {"network", "close", "network.http", close_request}};
static bool binary_allowed(const q2_caller_t *c) {
    if (!c || !c->instance || c->grant_count > Q2_CAPABILITY_MAX)
        return false;
    for (size_t i = 0; i < c->grant_count; ++i)
        if (c->grants[i] && !strcmp(c->grants[i], "network.http"))
            return true;
    return false;
}
static q2_result_t binary_get(void *ctx, const q2_caller_t *c, const char *url, unsigned timeout,
                              uint64_t *out_id) {
    if (!out_id || !url || strlen(url) >= Q2_TEXT_MAX)
        return Q2_ERR_ARGUMENT;
    *out_id = 0;
    if (!binary_allowed(c))
        return Q2_ERR_PERMISSION;
    q2_network_t *s = ctx;
    q2_value_t a[2] = {{.kind = Q2_STRING}, {.kind = Q2_INTEGER, .as.integer = timeout}}, out;
    strcpy(a[0].as.string, url);
    q2_result_t e = get_request(s->kernel, s, c, a, 2, &out, true);
    if (e == Q2_OK)
        *out_id = (uint64_t)out.as.integer;
    return e;
}
static q2_result_t binary_read(void *ctx, const q2_caller_t *c, uint64_t id, q2_network_response_t *out) {
    if (!out || id > INT64_MAX)
        return Q2_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!binary_allowed(c))
        return Q2_ERR_PERMISSION;
    q2_value_t a = {.kind = Q2_INTEGER, .as.integer = (int64_t)id};
    request_t *r = find(ctx, c, &a, 1);
    if (!r || !r->binary)
        return Q2_ERR_NOT_FOUND;
    if (r->result == Q2_OK)
        *out = r->response;
    return r->result;
}
static q2_result_t binary_close(void *ctx, const q2_caller_t *c, uint64_t id) {
    if (!binary_allowed(c))
        return Q2_ERR_PERMISSION;
    if (id > INT64_MAX)
        return Q2_ERR_ARGUMENT;
    q2_value_t a = {.kind = Q2_INTEGER, .as.integer = (int64_t)id};
    request_t *r = find(ctx, c, &a, 1);
    return r && r->binary ? clear(ctx, r) : Q2_ERR_NOT_FOUND;
}
q2_result_t q2_network_create(q2_kernel_t *k, const q2_network_backend_t *b, q2_network_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!k || !b || b->abi != 2 || b->size != sizeof(*b) || !b->start || !b->poll || !b->release)
        return Q2_ERR_ARGUMENT;
    q2_network_t *s = calloc(1, sizeof(*s));
    if (!s)
        return Q2_ERR_MEMORY;
    s->kernel = k;
    s->backend = *b;
    s->binary = (q2_network_binary_t){s, binary_get, binary_read, binary_close};
    s->service = (q2_service_t){"network.binary", 1, sizeof(s->binary), &s->binary};
    s->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = "network",
                                    .required = true,
                                    .context = s,
                                    .start = start,
                                    .stop = stop,
                                    .poll = poll,
                                    .release_app = release_app,
                                    .services = &s->service,
                                    .service_count = 1,
                                    .methods = methods,
                                    .method_count = sizeof(methods) / sizeof(*methods)};
    *out = s;
    return Q2_OK;
}
q2_result_t q2_network_component(q2_network_t *s, const q2_component_t **out) {
    if (!s || !out)
        return Q2_ERR_ARGUMENT;
    *out = &s->component;
    return Q2_OK;
}
q2_result_t q2_network_destroy(q2_network_t **s) {
    if (!s)
        return Q2_ERR_ARGUMENT;
    if (!*s)
        return Q2_OK;
    if ((*s)->ready)
        return Q2_ERR_STATE;
    free(*s);
    *s = NULL;
    return Q2_OK;
}
