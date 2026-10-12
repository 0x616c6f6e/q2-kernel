#include "q2/keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct q2_keys {
    q2_kernel_t *kernel;
    q2_component_t component;
    q2_key_state_t states[Q2_KEYS_MAX];
    size_t count;
    uint64_t subscribers[Q2_RUNTIME_MAX], router, foreground;
    q2_key_action_t delivered[Q2_RUNTIME_MAX][Q2_KEYS_MAX];
    bool ready, sealed;
    q2_data_t *data;
    unsigned data_handles[Q2_KEYS_MAX];
};
static bool letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool valid(const char *name) {
    if (!name || !letter((unsigned char)name[0]))
        return false;
    size_t n = strlen(name);
    if (n > Q2_KEY_NAME_MAX)
        return false;
    for (size_t i = 1; i < n; i++)
        if (!letter((unsigned char)name[i]) && !(name[i] >= '0' && name[i] <= '9') && name[i] != '_' &&
            name[i] != '-' && name[i] != '.')
            return false;
    return true;
}
static int find(q2_keys_t *keys, const char *name) {
    for (size_t i = 0; i < keys->count; i++)
        if (!strcmp(keys->states[i].name, name))
            return (int)i;
    return -1;
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_keys_t *keys = ctx;
    keys->sealed = keys->ready = true;
    return Q2_OK;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_keys_t *keys = ctx;
    memset(keys->subscribers, 0, sizeof(keys->subscribers));
    keys->ready = false;
    return Q2_OK;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    (void)k;
    q2_keys_t *keys = ctx;
    for (unsigned i = 0; i < Q2_RUNTIME_MAX; i++)
        if (keys->subscribers[i] == instance)
            keys->subscribers[i] = 0;
    return Q2_OK;
}
static q2_result_t count(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_INTEGER;
    out->as.integer = ((q2_keys_t *)ctx)->count;
    return Q2_OK;
}
static q2_result_t name(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)c;
    q2_keys_t *keys = ctx;
    if (n != 1 || a[0].kind != Q2_INTEGER || a[0].as.integer < 1 || (uint64_t)a[0].as.integer > keys->count)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_STRING;
    strcpy(out->as.string, keys->states[a[0].as.integer - 1].name);
    return Q2_OK;
}
static q2_result_t last(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)c;
    q2_keys_t *keys = ctx;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    int i = find(keys, a[0].as.string);
    if (i < 0)
        return Q2_ERR_NOT_FOUND;
    out->kind = Q2_INTEGER;
    out->as.integer = keys->states[i].last_action;
    if (keys->router && c->instance != keys->router) {
        /* Legacy snapshot API must not reveal a raw gesture withheld by the lifecycle router. */
        out->as.integer = Q2_KEY_NONE;
        for (unsigned slot = 0; slot < Q2_RUNTIME_MAX; slot++)
            if (keys->subscribers[slot] == c->instance)
                out->as.integer = keys->delivered[slot][i];
    }
    return Q2_OK;
}
static q2_result_t watch(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)out;
    q2_keys_t *keys = ctx;
    if (n != 1 || a[0].kind != Q2_BOOLEAN)
        return Q2_ERR_ARGUMENT;
    unsigned empty = Q2_RUNTIME_MAX;
    for (unsigned i = 0; i < Q2_RUNTIME_MAX; i++) {
        if (keys->subscribers[i] == c->instance) {
            if (!a[0].as.boolean)
                keys->subscribers[i] = 0;
            return Q2_OK;
        }
        if (!keys->subscribers[i])
            empty = i;
    }
    if (a[0].as.boolean) {
        if (empty == Q2_RUNTIME_MAX)
            return Q2_ERR_LIMIT;
        keys->subscribers[empty] = c->instance;
        memset(keys->delivered[empty], 0, sizeof(keys->delivered[empty]));
    }
    return Q2_OK;
}
static q2_result_t forward(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                           q2_value_t *out) {
    (void)out;
    q2_keys_t *keys = ctx;
    if (!keys->router || c->instance != keys->router)
        return Q2_ERR_PERMISSION;
    if (n != 2 || a[0].kind != Q2_STRING || a[1].kind != Q2_INTEGER || a[1].as.integer < 1 ||
        a[1].as.integer > 3)
        return Q2_ERR_ARGUMENT;
    if (find(keys, a[0].as.string) < 0)
        return Q2_ERR_NOT_FOUND;
    if (keys->data) {
        q2_value_t value = {.kind = Q2_INTEGER, .as.integer = a[1].as.integer};
        q2_result_t result =
            q2_data_publish(keys->data, keys, keys->data_handles[find(keys, a[0].as.string)], &value, Q2_OK);
        if (result != Q2_OK)
            return result;
    }
    if (keys->foreground == keys->router)
        return Q2_OK;
    for (unsigned i = 0; i < Q2_RUNTIME_MAX; i++)
        if (keys->subscribers[i] && keys->subscribers[i] == keys->foreground) {
            q2_event_t e = {.instance = keys->foreground, .topic = "key.action", .value = a[1].as.integer};
            strcpy(e.text, a[0].as.string);
            q2_result_t result = q2_event_post(k, &e);
            if (result == Q2_OK)
                keys->delivered[i][find(keys, a[0].as.string)] = (q2_key_action_t)a[1].as.integer;
            return result;
        }
    return Q2_OK;
}
static const q2_method_t methods[] = {{"keys", "forward", "system.lifecycle", forward},
                                      {"keys", "count", "input.keys", count},
                                      {"keys", "name", "input.keys", name},
                                      {"keys", "last", "input.keys", last},
                                      {"keys", "watch", "input.keys", watch}};
q2_result_t q2_keys_create(q2_kernel_t *k, q2_keys_t **out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = calloc(1, sizeof(q2_keys_t));
    if (!*out)
        return Q2_ERR_MEMORY;
    q2_keys_t *keys = *out;
    keys->kernel = k;
    keys->component = (q2_component_t){.abi = 1,
                                       .size = sizeof(q2_component_t),
                                       .id = "keys",
                                       .required = true,
                                       .context = keys,
                                       .start = start,
                                       .stop = stop,
                                       .release_app = release_app,
                                       .methods = methods,
                                       .method_count = sizeof(methods) / sizeof(*methods)};
    return Q2_OK;
}
q2_result_t q2_keys_register(q2_keys_t *keys, const char *name) {
    if (!keys || !valid(name))
        return Q2_ERR_ARGUMENT;
    if (keys->sealed || keys->data)
        return Q2_ERR_STATE;
    if (find(keys, name) >= 0)
        return Q2_ERR_EXISTS;
    if (keys->count == Q2_KEYS_MAX)
        return Q2_ERR_LIMIT;
    strcpy(keys->states[keys->count++].name, name);
    return Q2_OK;
}
q2_result_t q2_keys_component(q2_keys_t *keys, const q2_component_t **out) {
    if (!keys || !out)
        return Q2_ERR_ARGUMENT;
    *out = &keys->component;
    return Q2_OK;
}
q2_result_t q2_keys_report(q2_keys_t *keys, const char *name, q2_key_action_t action) {
    if (!keys || !name || action < Q2_KEY_SINGLE || action > Q2_KEY_LONG)
        return Q2_ERR_ARGUMENT;
    if (!keys->ready)
        return Q2_ERR_STATE;
    int i = find(keys, name);
    if (i < 0)
        return Q2_ERR_NOT_FOUND;
    keys->states[i].last_action = action;
    q2_result_t result = Q2_OK;
    if (keys->router) {
        q2_event_t event = {.instance = keys->router, .topic = "key.action", .value = action};
        strcpy(event.text, name);
        return q2_event_post(keys->kernel, &event);
    }
    if (keys->data) {
        q2_value_t value = {.kind = Q2_INTEGER, .as.integer = action};
        result = q2_data_publish(keys->data, keys, keys->data_handles[i], &value, Q2_OK);
    }
    for (unsigned j = 0; j < Q2_RUNTIME_MAX; j++)
        if (keys->subscribers[j]) {
            q2_event_t event = {.instance = keys->subscribers[j], .topic = "key.action", .value = action};
            strcpy(event.text, name);
            q2_result_t e = q2_event_post(keys->kernel, &event);
            if (result == Q2_OK)
                result = e;
        }
    return result;
}
q2_result_t q2_keys_count(q2_keys_t *keys, size_t *out) {
    if (!keys || !out)
        return Q2_ERR_ARGUMENT;
    *out = keys->count;
    return Q2_OK;
}
q2_result_t q2_keys_at(q2_keys_t *keys, size_t index, q2_key_state_t *out) {
    if (!keys || !out)
        return Q2_ERR_ARGUMENT;
    if (index >= keys->count)
        return Q2_ERR_NOT_FOUND;
    *out = keys->states[index];
    return Q2_OK;
}
q2_result_t q2_keys_destroy(q2_keys_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    if ((*ref)->ready)
        return Q2_ERR_BUSY;
    free(*ref);
    *ref = NULL;
    return Q2_OK;
}

q2_result_t q2_keys_route(q2_keys_t *keys, uint64_t lifecycle, uint64_t foreground) {
    if (!keys || !lifecycle || !foreground)
        return Q2_ERR_ARGUMENT;
    keys->router = lifecycle;
    keys->foreground = foreground;
    return keys->data ? q2_data_focus(keys->data, foreground) : Q2_OK;
}

q2_result_t q2_keys_data(q2_keys_t *keys, q2_data_t *data) {
    if (!keys || !data)
        return Q2_ERR_ARGUMENT;
    if (keys->ready || keys->data)
        return Q2_ERR_STATE;
    /* Bind once; a registration failure requires rebuilding the pre-boot objects. */
    keys->data = data;
    for (unsigned i = 0; i < keys->count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "input.key.%s.action", keys->states[i].name);
        q2_data_spec_t spec = {.name = name,
                               .capability = "input.keys",
                               .unit = "action",
                               .kind = Q2_DATA_EVENT,
                               .type = Q2_INTEGER,
                               .bounded = true,
                               .minimum = 1,
                               .maximum = 3,
                               .foreground_only = true};
        q2_result_t e = q2_data_register(data, keys, &spec, &keys->data_handles[i]);
        if (e != Q2_OK)
            return e;
    }
    return Q2_OK;
}
