#include "q2/data.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[64], capability[48], unit[16];
    const void *owner;
    q2_data_spec_t spec;
    q2_value_t value;
    q2_result_t status;
    uint64_t sequence, timestamp;
} entry_t;
typedef struct {
    uint64_t id, instance, seen, last_sent;
    unsigned entry;
    uint32_t interval;
    bool initial, sent;
} subscription_t;
struct q2_data {
    q2_kernel_t *kernel;
    q2_component_t component;
    entry_t entries[Q2_DATA_MAX];
    subscription_t subscriptions[Q2_DATA_SUB_MAX];
    const char *capabilities[Q2_CAPABILITY_MAX];
    unsigned count, cursor;
    uint64_t next_subscription, foreground;
    bool ready, sealed;
};
static bool name_valid(const char *s, size_t max, bool upper) {
    if (!s || !*s || strlen(s) > max)
        return false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        bool letter = (c >= 'a' && c <= 'z') || (upper && c >= 'A' && c <= 'Z');
        if (!letter && !(i && ((c >= '0' && c <= '9') || c == '_' || c == '.' || (upper && c == '-'))))
            return false;
    }
    return true;
}
static bool permitted(const q2_caller_t *caller, const char *cap) {
    for (size_t i = 0; i < caller->grant_count; i++)
        if (!strcmp(caller->grants[i], cap))
            return true;
    return false;
}
static q2_result_t lookup(q2_data_t *d, const q2_caller_t *caller, const q2_value_t *name, entry_t **out) {
    if (name->kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    for (unsigned i = 0; i < d->count; i++)
        if (!strcmp(d->entries[i].name, name->as.string)) {
            if (!permitted(caller, d->entries[i].capability))
                return Q2_ERR_PERMISSION;
            *out = &d->entries[i];
            return Q2_OK;
        }
    return Q2_ERR_NOT_FOUND;
}
static void expire(entry_t *e, uint64_t now) {
    if (e->spec.kind == Q2_DATA_STATE && e->status == Q2_OK && e->spec.ttl_ms &&
        now - e->timestamp >= e->spec.ttl_ms) {
        e->status = Q2_ERR_TIMEOUT;
        e->sequence++;
    }
}
static q2_result_t notify(q2_data_t *d, const subscription_t *s, const entry_t *entry) {
    q2_event_t event = {.instance = s->instance,
                        .has_data = true,
                        .coalesce = entry->spec.kind == Q2_DATA_STATE,
                        .data = entry->value,
                        .data_status = entry->status,
                        .subscription = s->id,
                        .sequence = entry->sequence,
                        .timestamp_ms = entry->timestamp};
    strcpy(event.key, entry->name);
    strcpy(event.topic, entry->spec.kind == Q2_DATA_STATE ? "data.changed" : "data.event");
    if (entry->status != Q2_OK)
        memset(&event.data, 0, sizeof(event.data));
    return q2_event_post(d->kernel, &event);
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_data_t *d = ctx;
    d->ready = d->sealed = true;
    return Q2_OK;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    q2_data_t *d = ctx;
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++)
        if (d->subscriptions[i].instance == instance) {
            q2_event_cancel(k, instance, d->subscriptions[i].id);
            memset(&d->subscriptions[i], 0, sizeof(subscription_t));
        }
    return Q2_OK;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    q2_data_t *d = ctx;
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++)
        if (d->subscriptions[i].id)
            q2_event_cancel(k, d->subscriptions[i].instance, d->subscriptions[i].id);
    memset(d->subscriptions, 0, sizeof(d->subscriptions));
    d->ready = false;
    return Q2_OK;
}
static q2_result_t poll(q2_kernel_t *k, void *ctx) {
    q2_data_t *d = ctx;
    uint64_t now;
    q2_kernel_time(k, &now);
    for (unsigned i = 0; i < d->count; i++)
        expire(&d->entries[i], now);
    /* Rotate fairly when the shared queue is full; retained states retry next owner poll. */
    unsigned start_index = d->cursor;
    for (unsigned n = 0; n < Q2_DATA_SUB_MAX; n++) {
        unsigned i = (start_index + n) % Q2_DATA_SUB_MAX;
        subscription_t *s = &d->subscriptions[i];
        if (!s->id)
            continue;
        entry_t *e = &d->entries[s->entry];
        if (e->spec.kind != Q2_DATA_STATE || (!s->initial && s->seen == e->sequence))
            continue;
        if (!s->initial && s->sent && now - s->last_sent < s->interval)
            continue;
        q2_result_t result = notify(d, s, e);
        if (result == Q2_ERR_LIMIT) {
            d->cursor = i;
            return Q2_OK;
        }
        if (result != Q2_OK)
            return result;
        s->initial = false;
        s->sent = true;
        s->seen = e->sequence;
        s->last_sent = now;
    }
    d->cursor = (start_index + 1) % Q2_DATA_SUB_MAX;
    return Q2_OK;
}
static q2_result_t get(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *a, size_t n,
                       q2_value_t *out) {
    if (n != 1)
        return Q2_ERR_ARGUMENT;
    q2_data_t *d = ctx;
    entry_t *e;
    q2_result_t r = lookup(d, caller, a, &e);
    if (r != Q2_OK)
        return r;
    if (e->spec.kind != Q2_DATA_STATE)
        return Q2_ERR_UNSUPPORTED;
    uint64_t now;
    q2_kernel_time(k, &now);
    expire(e, now);
    if (e->status != Q2_OK)
        return e->status;
    *out = e->value;
    return Q2_OK;
}
static q2_result_t info(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    if (n != 2 || a[1].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    entry_t *e;
    q2_result_t r = lookup(ctx, caller, a, &e);
    if (r != Q2_OK)
        return r;
    uint64_t now;
    q2_kernel_time(k, &now);
    expire(e, now);
    const char *field = a[1].as.string, *text = NULL;
    if (!strcmp(field, "kind"))
        text = e->spec.kind == Q2_DATA_STATE ? "state" : "event";
    else if (!strcmp(field, "type"))
        text = e->spec.type == Q2_BOOLEAN ? "boolean" : e->spec.type == Q2_INTEGER ? "integer" : "string";
    else if (!strcmp(field, "unit"))
        text = e->unit;
    else if (!strcmp(field, "capability"))
        text = e->capability;
    else if (!strcmp(field, "status") && e->spec.kind == Q2_DATA_STATE)
        q2_result_name(e->status, &text);
    else if (!strcmp(field, "timestamp_ms") && e->spec.kind == Q2_DATA_STATE) {
        out->kind = Q2_INTEGER;
        out->as.integer = (int64_t)e->timestamp;
        return Q2_OK;
    } else if (!strcmp(field, "sequence") && e->spec.kind == Q2_DATA_STATE) {
        out->kind = Q2_INTEGER;
        out->as.integer = (int64_t)e->sequence;
        return Q2_OK;
    } else
        return Q2_ERR_UNSUPPORTED;
    out->kind = Q2_STRING;
    strcpy(out->as.string, text);
    return Q2_OK;
}
static q2_result_t subscribe(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *a,
                             size_t n, q2_value_t *out) {
    (void)k;
    if (n < 1 || n > 3 || (n >= 2 && a[1].kind != Q2_BOOLEAN) ||
        (n == 3 && (a[2].kind != Q2_INTEGER || a[2].as.integer < 0 || a[2].as.integer > 3600000)))
        return Q2_ERR_ARGUMENT;
    q2_data_t *d = ctx;
    entry_t *e;
    q2_result_t r = lookup(d, caller, a, &e);
    if (r != Q2_OK)
        return r;
    if (e->spec.kind == Q2_DATA_EVENT && n == 3 && a[2].as.integer)
        return Q2_ERR_ARGUMENT;
    unsigned count = 0, empty = Q2_DATA_SUB_MAX, index = (unsigned)(e - d->entries);
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++) {
        subscription_t *s = &d->subscriptions[i];
        if (!s->id)
            empty = i;
        if (s->instance == caller->instance) {
            count++;
            if (s->entry == index)
                return Q2_ERR_EXISTS;
        }
    }
    if (count == Q2_DATA_APP_SUB_MAX || empty == Q2_DATA_SUB_MAX || d->next_subscription == INT64_MAX)
        return Q2_ERR_LIMIT;
    subscription_t *s = &d->subscriptions[empty];
    *s = (subscription_t){.id = ++d->next_subscription,
                          .instance = caller->instance,
                          .entry = index,
                          .seen = e->sequence,
                          .initial = e->spec.kind == Q2_DATA_STATE && (n < 2 || a[1].as.boolean),
                          .interval = n == 3 ? (uint32_t)a[2].as.integer : 0};
    out->kind = Q2_INTEGER;
    out->as.integer = (int64_t)s->id;
    return Q2_OK;
}
static q2_result_t unsubscribe(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *a,
                               size_t n, q2_value_t *out) {
    (void)out;
    if (n != 1 || a[0].kind != Q2_INTEGER || a[0].as.integer <= 0)
        return Q2_ERR_ARGUMENT;
    q2_data_t *d = ctx;
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++) {
        subscription_t *s = &d->subscriptions[i];
        if (s->id == (uint64_t)a[0].as.integer && s->instance == caller->instance) {
            q2_event_cancel(k, s->instance, s->id);
            memset(s, 0, sizeof(*s));
            return Q2_OK;
        }
    }
    return Q2_ERR_NOT_FOUND;
}
static const q2_method_t methods[] = {{"data", "get", "data.read", get},
                                      {"data", "info", "data.read", info},
                                      {"data", "subscribe", "data.read", subscribe},
                                      {"data", "unsubscribe", "data.read", unsubscribe}};
q2_result_t q2_data_create(q2_kernel_t *kernel, q2_data_t **out) {
    if (!kernel || !out)
        return Q2_ERR_ARGUMENT;
    *out = calloc(1, sizeof(q2_data_t));
    if (!*out)
        return Q2_ERR_MEMORY;
    q2_data_t *d = *out;
    d->kernel = kernel;
    d->component = (q2_component_t){.abi = Q2_COMPONENT_ABI,
                                    .size = sizeof(q2_component_t),
                                    .id = "data",
                                    .required = true,
                                    .context = d,
                                    .start = start,
                                    .stop = stop,
                                    .poll = poll,
                                    .release_app = release_app,
                                    .methods = methods,
                                    .method_count = sizeof(methods) / sizeof(*methods),
                                    .capabilities = d->capabilities};
    return Q2_OK;
}
q2_result_t q2_data_register(q2_data_t *d, const void *owner, const q2_data_spec_t *s, unsigned *handle) {
    if (!d || !owner || !s || !handle || !name_valid(s->name, 63, true) ||
        !name_valid(s->capability, 47, false) || !s->unit || strlen(s->unit) > 15 ||
        s->kind > Q2_DATA_EVENT || s->kind < Q2_DATA_STATE || s->type < Q2_BOOLEAN || s->type > Q2_STRING ||
        (s->bounded && (s->type != Q2_INTEGER || s->minimum > s->maximum)) ||
        (s->foreground_only && s->kind != Q2_DATA_EVENT) || (s->ttl_ms && s->kind != Q2_DATA_STATE))
        return Q2_ERR_ARGUMENT;
    if (d->sealed)
        return Q2_ERR_STATE;
    for (unsigned i = 0; i < d->count; i++)
        if (!strcmp(d->entries[i].name, s->name))
            return Q2_ERR_EXISTS;
    if (d->count == Q2_DATA_MAX)
        return Q2_ERR_LIMIT;
    bool new_cap = true;
    for (size_t i = 0; i < d->component.capability_count; i++)
        if (!strcmp(d->capabilities[i], s->capability))
            new_cap = false;
    if (new_cap && d->component.capability_count == Q2_CAPABILITY_MAX)
        return Q2_ERR_LIMIT;
    entry_t *e = &d->entries[d->count];
    strcpy(e->name, s->name);
    strcpy(e->capability, s->capability);
    strcpy(e->unit, s->unit);
    e->spec = *s;
    e->spec.name = e->name;
    e->spec.capability = e->capability;
    e->spec.unit = e->unit;
    e->owner = owner;
    e->status = Q2_ERR_STATE;
    if (new_cap)
        d->capabilities[d->component.capability_count++] = e->capability;
    *handle = ++d->count;
    return Q2_OK;
}
q2_result_t q2_data_component(q2_data_t *d, const q2_component_t **out) {
    if (!d || !out)
        return Q2_ERR_ARGUMENT;
    *out = &d->component;
    return Q2_OK;
}
q2_result_t q2_data_publish(q2_data_t *d, const void *owner, unsigned handle, const q2_value_t *v,
                            q2_result_t status) {
    if (!d || !owner || !handle || handle > d->count || status > Q2_OK || status < Q2_ERR_COMPONENT)
        return Q2_ERR_ARGUMENT;
    if (!d->ready)
        return Q2_ERR_STATE;
    entry_t *e = &d->entries[handle - 1];
    if (e->owner != owner)
        return Q2_ERR_PERMISSION;
    if (e->spec.kind == Q2_DATA_EVENT && status != Q2_OK)
        return Q2_ERR_ARGUMENT;
    if (status == Q2_OK &&
        (!v || v->kind != e->spec.type || (v->kind == Q2_STRING && !memchr(v->as.string, 0, Q2_TEXT_MAX)) ||
         (e->spec.bounded && (v->as.integer < e->spec.minimum || v->as.integer > e->spec.maximum))))
        return Q2_ERR_ARGUMENT;
    uint64_t now;
    q2_kernel_time(d->kernel, &now);
    bool changed = e->status != status;
    if (status == Q2_OK) {
        changed |= v->kind == Q2_BOOLEAN   ? e->value.as.boolean != v->as.boolean
                   : v->kind == Q2_INTEGER ? e->value.as.integer != v->as.integer
                                           : strcmp(e->value.as.string, v->as.string) != 0;
        e->value = *v;
    } else
        memset(&e->value, 0, sizeof(e->value));
    e->status = status;
    e->timestamp = now;
    if (changed || e->spec.kind == Q2_DATA_EVENT)
        e->sequence++;
    if (e->spec.kind == Q2_DATA_STATE)
        return Q2_OK;
    q2_result_t result = Q2_OK;
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++) {
        subscription_t *s = &d->subscriptions[i];
        if (!s->id || s->entry != handle - 1 || (e->spec.foreground_only && s->instance != d->foreground))
            continue;
        q2_result_t r = notify(d, s, e);
        if (result == Q2_OK)
            result = r;
    }
    return result;
}
q2_result_t q2_data_focus(q2_data_t *d, uint64_t instance) {
    if (!d)
        return Q2_ERR_ARGUMENT;
    if (d->foreground == instance)
        return Q2_OK;
    d->foreground = instance;
    for (unsigned i = 0; i < Q2_DATA_SUB_MAX; i++) {
        subscription_t *s = &d->subscriptions[i];
        if (s->id && d->entries[s->entry].spec.foreground_only && s->instance != instance)
            q2_event_cancel(d->kernel, s->instance, s->id);
    }
    return Q2_OK;
}
q2_result_t q2_data_destroy(q2_data_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    if ((*ref)->ready)
        return Q2_ERR_BUSY;
    free(*ref);
    *ref = NULL;
    return Q2_OK;
}
