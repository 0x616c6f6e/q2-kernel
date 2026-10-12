#include "internal.h"
#include <string.h>
static int find(q2_kernel_t *k, const char *id) {
    if (id)
        for (size_t i = 0; i < k->count; i++)
            if (!strcmp(k->components[i]->id, id))
                return (int)i;
    return -1;
}
static bool identifier(const char *s) {
    if (!s || !*s || strlen(s) > 47)
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.'))
            return false;
    return true;
}
q2_result_t q2_kernel_create(const q2_port_t *p, q2_kernel_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!p || p->abi != Q2_PORT_ABI || p->size < sizeof(*p) || !p->allocate || !p->release || !p->now_ms)
        return Q2_ERR_ARGUMENT;
    q2_kernel_t *k = p->allocate(p->context, sizeof(*k));
    if (!k)
        return Q2_ERR_MEMORY;
    memset(k, 0, sizeof(*k));
    k->port = *p;
    *out = k;
    return Q2_OK;
}
q2_result_t q2_component_register(q2_kernel_t *k, const q2_component_t *c) {
    if (!k || !c || c->abi != Q2_COMPONENT_ABI || c->size < sizeof(*c) || !identifier(c->id) || !c->start ||
        !c->stop || c->method_count > 32 || c->service_count > 4 || (c->method_count && !c->methods) ||
        (c->service_count && !c->services) || c->capability_count > Q2_CAPABILITY_MAX ||
        (c->capability_count && !c->capabilities))
        return Q2_ERR_ARGUMENT;
    if (k->booted)
        return Q2_ERR_STATE;
    if (find(k, c->id) >= 0)
        return Q2_ERR_EXISTS;
    if (k->count == Q2_COMPONENT_MAX)
        return Q2_ERR_LIMIT;
    for (size_t i = 0; i < c->capability_count; i++)
        if (!identifier(c->capabilities[i]))
            return Q2_ERR_ARGUMENT;
    /* Reject conflicting exported contracts before publishing the descriptor. */
    for (size_t i = 0; i < c->method_count; i++) {
        const q2_method_t *m = &c->methods[i];
        if (!identifier(m->module) || strchr(m->module, '.') || !strcmp(m->module, "os") ||
            !identifier(m->name) || !identifier(m->capability) || !m->invoke)
            return Q2_ERR_ARGUMENT;
        for (size_t p = 0; p <= k->count; p++) {
            const q2_component_t *old = p == k->count ? c : k->components[p];
            size_t limit = p == k->count ? i : old->method_count;
            for (size_t j = 0; j < limit; j++)
                if (!strcmp(m->module, old->methods[j].module) && !strcmp(m->name, old->methods[j].name))
                    return Q2_ERR_EXISTS;
        }
    }
    for (size_t i = 0; i < c->service_count; i++) {
        const q2_service_t *s = &c->services[i];
        if (!identifier(s->name) || !s->abi || !s->size || !s->api)
            return Q2_ERR_ARGUMENT;
        for (size_t p = 0; p <= k->count; p++) {
            const q2_component_t *old = p == k->count ? c : k->components[p];
            for (size_t j = 0; j < (p == k->count ? i : old->service_count); j++)
                if (!strcmp(s->name, old->services[j].name))
                    return Q2_ERR_EXISTS;
        }
    }
    k->components[k->count++] = c;
    Q2_LOGD(k, "component", "registered %s", c->id);
    return Q2_OK;
}
q2_result_t q2_kernel_boot(q2_kernel_t *k) {
    if (!k)
        return Q2_ERR_ARGUMENT;
    if (k->booted)
        return Q2_ERR_STATE;
    Q2_LOGI(k, "kernel", "boot components=%u", (unsigned)k->count);
    uint32_t edges[Q2_COMPONENT_MAX] = {0}, done = 0;
    /* Validate the entire graph without starting any device or runtime. */
    for (size_t i = 0; i < k->count; i++)
        for (size_t d = 0; d < Q2_DEPENDENCY_MAX; d++) {
            int n = find(k, k->components[i]->needs[d]);
            if (k->components[i]->needs[d] && n < 0) {
                Q2_LOGE(k, "kernel", "missing dependency for %s", k->components[i]->id);
                return Q2_ERR_DEPENDENCY;
            }
            if (n >= 0)
                edges[i] |= UINT32_C(1) << n;
            n = find(k, k->components[i]->after[d]);
            if (n >= 0)
                edges[i] |= UINT32_C(1) << n;
        }
    for (size_t n = 0; n < k->count; n++) {
        size_t i;
        for (i = 0; i < k->count; i++)
            if (!(done & (UINT32_C(1) << i)) && !(edges[i] & ~done))
                break;
        if (i == k->count) {
            Q2_LOGE(k, "kernel", "component dependency cycle");
            return Q2_ERR_CYCLE;
        }
        k->order[n] = i;
        done |= UINT32_C(1) << i;
    }
    k->booted = true;
    k->ordered = k->count;
    q2_result_t result = Q2_OK;
    for (size_t n = 0; n < k->count; n++) {
        size_t i = k->order[n];
        const q2_component_t *c = k->components[i];
        bool blocked = false;
        for (size_t d = 0; d < Q2_DEPENDENCY_MAX; d++)
            if (c->needs[d] && k->states[find(k, c->needs[d])] != Q2_READY)
                blocked = true;
        if (blocked) {
            k->states[i] = Q2_BLOCKED;
            k->errors[i] = Q2_ERR_DEPENDENCY;
        } else {
            k->states[i] = Q2_STARTING;
            k->errors[i] = c->start(k, c->context);
            k->states[i] = k->errors[i] == Q2_OK ? Q2_READY : Q2_FAILED;
        }
        if (c->required && k->states[i] != Q2_READY)
            result = Q2_ERR_COMPONENT;
        if (k->states[i] == Q2_READY)
            Q2_LOGI(k, "component", "%s ready", c->id);
        else
            Q2_LOGE(k, "component", "%s startup failed result=%d", c->id, k->errors[i]);
    }
    k->running = true;
    Q2_LOGI(k, "kernel", "boot complete result=%d", result);
    return result;
}
q2_result_t q2_kernel_poll(q2_kernel_t *k, size_t budget) {
    if (!k)
        return Q2_ERR_ARGUMENT;
    if (!k->running)
        return Q2_ERR_STATE;
    if (k->polling)
        return Q2_ERR_BUSY;
    k->polling = true;
    q2_result_t result = Q2_OK;
    for (size_t n = 0; n < k->ordered; n++) {
        size_t i = k->order[n];
        const q2_component_t *c = k->components[i];
        if (k->states[i] == Q2_READY && c->poll) {
            q2_result_t e = c->poll(k, c->context);
            if (e != Q2_OK && result == Q2_OK)
                result = e;
        }
    }
    k->component_poll_result = result;
    if (budget > Q2_EVENT_MAX)
        budget = Q2_EVENT_MAX;
    for (size_t n = 0; n < budget && k->used; n++) {
        q2_event_t event = k->events[k->head];
        k->head = (k->head + 1) % Q2_EVENT_MAX;
        k->used--;
        for (size_t i = 0; i < k->attached; i++) {
            q2_result_t e = k->runtimes[i].event(k->runtimes[i].context, &event);
            if (e != Q2_OK && result == Q2_OK)
                result = e;
        }
    }
    for (size_t i = 0; i < k->attached; i++) {
        q2_result_t e = k->runtimes[i].poll(k->runtimes[i].context);
        if (e != Q2_OK && result == Q2_OK)
            result = e;
    }
    k->polling = false;
    if (result != Q2_OK)
        Q2_LOGE(k, "kernel", "poll failed result=%d", result);
    return result;
}
q2_result_t q2_runtime_attach(q2_kernel_t *k, const q2_runtime_driver_t *r) {
    if (!k || !r || r->abi != 1 || r->size < sizeof(*r) || !r->event || !r->poll || !r->stop)
        return Q2_ERR_ARGUMENT;
    if (!k->running)
        return Q2_ERR_STATE;
    if (k->polling)
        return Q2_ERR_BUSY;
    for (size_t i = 0; i < k->attached; i++)
        if (k->runtimes[i].context == r->context)
            return Q2_ERR_EXISTS;
    if (k->attached == Q2_RUNTIME_MAX)
        return Q2_ERR_LIMIT;
    k->runtimes[k->attached++] = *r;
    return Q2_OK;
}
q2_result_t q2_runtime_detach(q2_kernel_t *k, void *ctx) {
    if (!k)
        return Q2_ERR_ARGUMENT;
    if (k->polling)
        return Q2_ERR_BUSY;
    size_t i = 0;
    while (i < k->attached && k->runtimes[i].context != ctx)
        i++;
    if (i == k->attached)
        return Q2_ERR_NOT_FOUND;
    q2_result_t e = k->runtimes[i].stop(ctx);
    if (e != Q2_OK)
        return e;
    memmove(k->runtimes + i, k->runtimes + i + 1, (k->attached - i - 1) * sizeof(*k->runtimes));
    memset(&k->runtimes[--k->attached], 0, sizeof(*k->runtimes));
    return Q2_OK;
}
q2_result_t q2_kernel_release_app(q2_kernel_t *k, uint64_t instance) {
    if (!k || !instance)
        return Q2_ERR_ARGUMENT;
    q2_result_t result = Q2_OK;
    for (size_t n = k->ordered; n; n--) {
        size_t i = k->order[n - 1];
        const q2_component_t *c = k->components[i];
        if (k->states[i] == Q2_READY && c->release_app) {
            q2_result_t e = c->release_app(k, c->context, instance);
            if (e != Q2_OK && result == Q2_OK)
                result = e;
        }
    }
    return result;
}
q2_result_t q2_kernel_next_instance(q2_kernel_t *k, uint64_t *out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    if (k->next_instance == UINT64_MAX)
        return Q2_ERR_LIMIT;
    *out = ++k->next_instance;
    return Q2_OK;
}
q2_result_t q2_kernel_destroy(q2_kernel_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    q2_kernel_t *k = *ref;
#if Q2_ENABLE_LOG
    if (k->logging)
        return Q2_ERR_BUSY;
#endif
    if (k->polling)
        return Q2_ERR_BUSY;
    /* Runtime objects belong to their owner; require explicit detach before
     * freeing the kernel. */
    if (k->attached)
        return Q2_ERR_BUSY;
    k->running = false;
    for (size_t n = k->ordered; n; n--) {
        size_t i = k->order[n - 1];
        if (k->states[i] == Q2_READY) {
            const q2_component_t *c = k->components[i];
            q2_result_t e = c->stop(k, c->context);
            if (e != Q2_OK) {
                Q2_LOGE(k, "component", "%s stop failed result=%d", c->id, e);
                return e;
            }
            k->states[i] = Q2_STOPPED;
            Q2_LOGI(k, "component", "%s stopped", c->id);
        }
    }
    q2_port_t p = k->port;
    Q2_LOGI(k, "kernel", "shutdown complete");
    p.release(p.context, k);
    *ref = NULL;
    return Q2_OK;
}
q2_result_t q2_event_post(q2_kernel_t *k, const q2_event_t *event) {
    if (!k || !event || !memchr(event->topic, 0, sizeof(event->topic)) ||
        !memchr(event->text, 0, sizeof(event->text)))
        return Q2_ERR_ARGUMENT;
    if (!k->running)
        return Q2_ERR_STATE;
    if (event->has_data) {
        if (!event->subscription || !event->instance || !memchr(event->key, 0, sizeof(event->key)) ||
            event->data.kind < Q2_NIL || event->data.kind > Q2_STRING ||
            (event->data.kind == Q2_STRING && !memchr(event->data.as.string, 0, Q2_TEXT_MAX)))
            return Q2_ERR_ARGUMENT;
        /* State subscriptions retain only their newest pending sample; edges are never merged. */
        if (event->coalesce)
            for (size_t i = 0; i < k->used; i++) {
                q2_event_t *pending = &k->events[(k->head + i) % Q2_EVENT_MAX];
                if (pending->has_data && pending->coalesce && pending->instance == event->instance &&
                    pending->subscription == event->subscription) {
                    *pending = *event;
                    return Q2_OK;
                }
            }
    }
    if (k->used == Q2_EVENT_MAX) {
        k->dropped++;
        /* Log only the first overflow; counters retain the full total without flooding the sink. */
        if (k->dropped == 1)
            Q2_LOGW(k, "event", "queue full; further drops counted silently");
        return Q2_ERR_LIMIT;
    }
    k->events[(k->head + k->used) % Q2_EVENT_MAX] = *event;
    k->used++;
    k->posted++;
    Q2_LOGD(k, "event", "posted %s instance=%llu", event->topic, (unsigned long long)event->instance);
    return Q2_OK;
}
q2_result_t q2_event_cancel(q2_kernel_t *k, uint64_t instance, uint64_t subscription) {
    if (!k || !instance || !subscription)
        return Q2_ERR_ARGUMENT;
    size_t kept = 0;
    for (size_t i = 0; i < k->used; i++) {
        q2_event_t *event = &k->events[(k->head + i) % Q2_EVENT_MAX];
        if (event->has_data && event->instance == instance && event->subscription == subscription)
            continue;
        k->events[(k->head + kept++) % Q2_EVENT_MAX] = *event;
    }
    k->used = kept;
    return Q2_OK;
}
q2_result_t q2_service_get(q2_kernel_t *k, const char *name, unsigned abi, size_t size, const void **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!k || !name || !abi || !size)
        return Q2_ERR_ARGUMENT;
    for (size_t i = 0; i < k->count; i++)
        if (k->states[i] == Q2_READY)
            for (size_t j = 0; j < k->components[i]->service_count; j++) {
                const q2_service_t *s = &k->components[i]->services[j];
                if (!strcmp(name, s->name)) {
                    if (s->abi != abi || s->size < size)
                        return Q2_ERR_UNSUPPORTED;
                    *out = s->api;
                    return Q2_OK;
                }
            }
    return Q2_ERR_NOT_FOUND;
}
static bool valid_value(const q2_value_t *value) {
    if (value->kind < Q2_NIL || value->kind > Q2_STRING)
        return false;
    return value->kind != Q2_STRING || memchr(value->as.string, 0, sizeof(value->as.string)) != NULL;
}
q2_result_t q2_kernel_call(q2_kernel_t *k, const q2_caller_t *caller, const char *module, const char *name,
                           const q2_value_t *args, size_t count, q2_value_t *out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!k || !caller || !module || !name || !caller->instance || count > 4 || (count && !args) ||
        caller->grant_count > Q2_CAPABILITY_MAX)
        return Q2_ERR_ARGUMENT;
    if (!k->running)
        return Q2_ERR_STATE;
    /* Validate bounded values before providers can inspect their string data. */
    for (size_t i = 0; i < count; i++)
        if (!valid_value(&args[i]))
            return Q2_ERR_ARGUMENT;
    for (size_t i = 0; i < k->count; i++) {
        const q2_component_t *c = k->components[i];
        for (size_t j = 0; j < c->method_count; j++) {
            const q2_method_t *m = &c->methods[j];
            if (strcmp(module, m->module) || strcmp(name, m->name))
                continue;
            if (k->states[i] != Q2_READY)
                return Q2_ERR_STATE;
            bool granted = false;
            for (size_t g = 0; g < caller->grant_count; g++)
                if (caller->grants[g] && !strcmp(m->capability, caller->grants[g]))
                    granted = true;
            if (!granted) {
                Q2_LOGW(k, "permission", "denied %s.%s instance=%llu", module, name,
                        (unsigned long long)caller->instance);
                return Q2_ERR_PERMISSION;
            }
            q2_result_t e = m->invoke(k, c->context, caller, args, count, out);
            if (e == Q2_OK && !valid_value(out))
                e = Q2_ERR_COMPONENT;
            if (e != Q2_OK)
                memset(out, 0, sizeof(*out));
            return e;
        }
    }
    return Q2_ERR_UNSUPPORTED;
}
q2_result_t q2_capability_available(q2_kernel_t *k, const char *name, bool *out) {
    if (!k || !name || !out)
        return Q2_ERR_ARGUMENT;
    *out = !strcmp(name, "system.info");
#if Q2_ENABLE_LOG
    if (!strcmp(name, "system.log"))
        *out = true;
#endif
    for (size_t i = 0; i < k->count; i++)
        if (k->states[i] == Q2_READY)
            for (size_t j = 0; j < k->components[i]->method_count; j++)
                if (!strcmp(name, k->components[i]->methods[j].capability))
                    *out = true;
    for (size_t i = 0; i < k->count; i++)
        if (k->states[i] == Q2_READY)
            for (size_t j = 0; j < k->components[i]->capability_count; j++)
                if (!strcmp(name, k->components[i]->capabilities[j]))
                    *out = true;
    return Q2_OK;
}
q2_result_t q2_method_at(q2_kernel_t *k, size_t index, const q2_method_t **out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    for (size_t i = 0; i < k->count; i++)
        if (k->states[i] == Q2_READY) {
            if (index < k->components[i]->method_count) {
                *out = &k->components[i]->methods[index];
                return Q2_OK;
            }
            index -= k->components[i]->method_count;
        }
    return Q2_ERR_NOT_FOUND;
}
q2_result_t q2_component_status(q2_kernel_t *k, size_t i, q2_component_status_t *out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    if (i >= k->count)
        return Q2_ERR_NOT_FOUND;
    *out = (q2_component_status_t){k->components[i]->id, k->states[i], k->errors[i]};
    return Q2_OK;
}
q2_result_t q2_kernel_status(q2_kernel_t *k, q2_kernel_status_t *out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out =
        (q2_kernel_status_t){k->count, k->used, k->posted, k->dropped, k->running, k->component_poll_result};
    return Q2_OK;
}
q2_result_t q2_kernel_time(q2_kernel_t *k, uint64_t *out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = k->port.now_ms(k->port.context);
    return Q2_OK;
}
q2_result_t q2_kernel_allocate(q2_kernel_t *k, size_t n, void **out) {
    if (!k || !out || !n)
        return Q2_ERR_ARGUMENT;
    *out = k->port.allocate(k->port.context, n);
    return *out ? Q2_OK : Q2_ERR_MEMORY;
}
q2_result_t q2_kernel_release(q2_kernel_t *k, void *p) {
    if (!k)
        return Q2_ERR_ARGUMENT;
    k->port.release(k->port.context, p);
    return Q2_OK;
}
