#include "q2/apps.h"
#include "q2/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    q2_manifest_t meta;
    char *manifest, *source;
    size_t manifest_size, source_size, grant_count;
    char grant_text[Q2_CAPABILITY_MAX][48];
    const char *grants[Q2_CAPABILITY_MAX];
    bool factory;
} package_t;
struct q2_apps {
    q2_kernel_t *kernel;
    q2_lua_runtime_t *runtime, *runtimes[Q2_APPS_MAX];
    q2_component_t component;
    q2_app_prepare_fn prepare;
    void *context;
    package_t packages[Q2_APPS_MAX];
    unsigned count;
    int home, current, pending;
    bool ready, persistent, sealed;
    q2_caller_t system;
    char background[32], last_error[Q2_TEXT_MAX];
};
static int find(q2_apps_t *m, const char *id) {
    for (unsigned i = 0; i < m->count; i++)
        if (!strcmp(m->packages[i].meta.id, id))
            return (int)i;
    return -1;
}
static q2_result_t authorized(q2_apps_t *m, int index) {
    package_t *p = &m->packages[index];
    for (size_t i = 0; i < p->meta.required_count; i++) {
        bool granted = false, available = false;
        for (size_t j = 0; j < p->grant_count; j++)
            if (!strcmp(p->meta.required[i], p->grants[j]))
                granted = true;
        if (!granted)
            return Q2_ERR_PERMISSION;
        q2_capability_available(m->kernel, p->meta.required[i], &available);
        if (!available)
            return Q2_ERR_UNSUPPORTED;
    }
    return Q2_OK;
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_apps_t *m = ctx;
    m->sealed = true;
    if (m->home < 0)
        return Q2_ERR_NOT_FOUND;
    m->ready = true;
    return Q2_OK;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    (void)k;
    ((q2_apps_t *)ctx)->ready = false;
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
    out->as.integer = ((q2_apps_t *)ctx)->count;
    return Q2_OK;
}
static q2_result_t id(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                      q2_value_t *out) {
    (void)k;
    (void)c;
    q2_apps_t *m = ctx;
    if (n != 1 || a[0].kind != Q2_INTEGER || a[0].as.integer < 1 || a[0].as.integer > m->count)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_STRING;
    strcpy(out->as.string, m->packages[a[0].as.integer - 1].meta.id);
    return Q2_OK;
}
static q2_result_t info(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)c;
    q2_apps_t *m = ctx;
    if (n != 2 || a[0].kind != Q2_STRING || a[1].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    int i = find(m, a[0].as.string);
    if (i < 0)
        return Q2_ERR_NOT_FOUND;
    package_t *p = &m->packages[i];
    out->kind = Q2_STRING;
    if (!strcmp(a[1].as.string, "name"))
        strcpy(out->as.string, p->meta.name);
    else if (!strcmp(a[1].as.string, "icon")) {
        if (*p->meta.icon)
            snprintf(out->as.string, sizeof(out->as.string), "assets/catalog-%s.png", p->meta.id);
        else
            strcpy(out->as.string, "assets/app-default.png");
    } else if (!strcmp(a[1].as.string, "factory")) {
        out->kind = Q2_BOOLEAN;
        out->as.boolean = p->factory;
    } else if (!strcmp(a[1].as.string, "home")) {
        out->kind = Q2_BOOLEAN;
        out->as.boolean = i == m->home;
    } else
        return Q2_ERR_ARGUMENT;
    return Q2_OK;
}
static q2_result_t launch(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    (void)c;
    (void)out;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    return q2_apps_request(ctx, a[0].as.string);
}
static q2_result_t home(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    (void)out;
    return n ? Q2_ERR_ARGUMENT : q2_apps_request(ctx, NULL);
}
static q2_result_t last_error(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_STRING;
    strcpy(out->as.string, ((q2_apps_t *)ctx)->last_error);
    return Q2_OK;
}
static bool valid_background(const char *value) {
    return !strcmp(value, "home-background") || !strcmp(value, "midnight") || !strcmp(value, "forest");
}
static q2_result_t get_background(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a,
                                  size_t n, q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_STRING;
    strcpy(out->as.string, ((q2_apps_t *)ctx)->background);
    return Q2_OK;
}
static q2_result_t persistent(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_BOOLEAN;
    out->as.boolean = ((q2_apps_t *)ctx)->persistent;
    return Q2_OK;
}
static q2_result_t set_background(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a,
                                  size_t n, q2_value_t *out) {
    q2_apps_t *m = ctx;
    if (n != 1 || a[0].kind != Q2_STRING || !valid_background(a[0].as.string))
        return Q2_ERR_ARGUMENT;
    int i = find(m, c->app_id);
    /* A downloaded manifest cannot self-declare factory status or change global appearance. */
    if (i < 0 || !m->packages[i].factory)
        return Q2_ERR_PERMISSION;
    if (m->persistent) {
        q2_value_t args[2] = {{.kind = Q2_STRING, .as.string = "system-background"}, a[0]};
        q2_result_t e = q2_kernel_call(k, &m->system, "storage", "put", args, 2, out);
        if (e != Q2_OK)
            return e;
    }
    strcpy(m->background, a[0].as.string);
    out->kind = Q2_BOOLEAN;
    out->as.boolean = true;
    return Q2_OK;
}
static q2_result_t foreground(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)c;
    (void)a;
    q2_apps_t *m = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    out->kind = Q2_STRING;
    strcpy(out->as.string, m->packages[m->current < 0 ? m->home : m->current].meta.id);
    return Q2_OK;
}
static const q2_method_t methods[] = {
    {"apps", "foreground", "apps.list", foreground},
    {"apps", "count", "apps.list", count},
    {"apps", "id", "apps.list", id},
    {"apps", "info", "apps.list", info},
    {"apps", "last_error", "apps.list", last_error},
    {"apps", "launch", "apps.launch", launch},
    {"apps", "home", "apps.navigate", home},
    {"appearance", "get", "appearance.read", get_background},
    {"appearance", "persistent", "appearance.read", persistent},
    {"appearance", "set", "appearance.write", set_background},
};
q2_result_t q2_apps_create(q2_kernel_t *k, q2_app_prepare_fn prepare, void *context, q2_apps_t **out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = calloc(1, sizeof(q2_apps_t));
    if (!*out)
        return Q2_ERR_MEMORY;
    q2_apps_t *m = *out;
    m->kernel = k;
    m->prepare = prepare;
    m->context = context;
    m->home = m->current = m->pending = -1;
    strcpy(m->background, "home-background");
    m->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = "apps",
                                    .required = true,
                                    .after = {"storage_littlefs", "display"},
                                    .context = m,
                                    .start = start,
                                    .stop = stop,
                                    .methods = methods,
                                    .method_count = sizeof(methods) / sizeof(*methods)};
    return Q2_OK;
}
q2_result_t q2_apps_register(q2_apps_t *m, const q2_app_package_t *input) {
    if (!m || !input || !input->manifest || !input->source || !input->source_size ||
        input->source_size > Q2_SOURCE_MAX || input->grant_count > Q2_CAPABILITY_MAX ||
        (input->grant_count && !input->grants) || (input->home && !input->factory))
        return Q2_ERR_ARGUMENT;
    if (m->sealed)
        return Q2_ERR_STATE;
    if (m->count == Q2_APPS_MAX)
        return Q2_ERR_LIMIT;
    q2_manifest_t meta;
    q2_result_t e = q2_manifest_parse(input->manifest, input->manifest_size, &meta);
    if (e != Q2_OK)
        return e;
    if (find(m, meta.id) >= 0 || (input->home && m->home >= 0))
        return Q2_ERR_EXISTS;
    for (size_t i = 0; i < input->grant_count; i++)
        if (!input->grants[i] || !*input->grants[i] || strlen(input->grants[i]) >= 48)
            return Q2_ERR_ARGUMENT;
    package_t *p = &m->packages[m->count];
    p->manifest = malloc(input->manifest_size + 1);
    p->source = malloc(input->source_size + 1);
    if (!p->manifest || !p->source) {
        free(p->manifest);
        free(p->source);
        memset(p, 0, sizeof(*p));
        return Q2_ERR_MEMORY;
    }
    memcpy(p->manifest, input->manifest, input->manifest_size);
    p->manifest[input->manifest_size] = 0;
    memcpy(p->source, input->source, input->source_size);
    p->source[input->source_size] = 0;
    p->meta = meta;
    p->manifest_size = input->manifest_size;
    p->source_size = input->source_size;
    p->factory = input->factory;
    p->grant_count = input->grant_count;
    for (size_t i = 0; i < p->grant_count; i++) {
        strcpy(p->grant_text[i], input->grants[i]);
        p->grants[i] = p->grant_text[i];
    }
    if (input->home)
        m->home = (int)m->count;
    m->count++;
    return Q2_OK;
}
q2_result_t q2_apps_component(q2_apps_t *m, const q2_component_t **out) {
    if (!m || !out)
        return Q2_ERR_ARGUMENT;
    *out = &m->component;
    return Q2_OK;
}
q2_result_t q2_apps_bind(q2_apps_t *m, q2_lua_runtime_t *r) {
    if (!m || !r)
        return Q2_ERR_ARGUMENT;
    if (!m->ready || m->runtime)
        return Q2_ERR_STATE;
    q2_app_status_t status;
    q2_lua_status(r, &status);
    if (status.instance)
        return Q2_ERR_BUSY;
    q2_result_t e = q2_kernel_next_instance(m->kernel, &m->system.instance);
    if (e != Q2_OK)
        return e;
    strcpy(m->system.app_id, m->packages[m->home].meta.id);
    m->system.grants[0] = "storage.private";
    m->system.grant_count = 1;
    q2_capability_available(m->kernel, "storage.private", &m->persistent);
    if (m->persistent) {
        q2_value_t key = {.kind = Q2_STRING, .as.string = "system-background"}, value;
        e = q2_kernel_call(m->kernel, &m->system, "storage", "get", &key, 1, &value);
        if (e == Q2_OK && value.kind == Q2_STRING && valid_background(value.as.string))
            strcpy(m->background, value.as.string);
        else if (e != Q2_OK && e != Q2_ERR_NOT_FOUND)
            return e;
    }
    m->runtime = r;
    m->runtimes[m->home] = r;
    return Q2_OK;
}
q2_result_t q2_apps_request(q2_apps_t *m, const char *app_id) {
    if (!m)
        return Q2_ERR_ARGUMENT;
    if (!m->ready || !m->runtime)
        return Q2_ERR_STATE;
    int target = app_id ? find(m, app_id) : m->home;
    if (target < 0)
        return Q2_ERR_NOT_FOUND;
    if (m->pending >= 0)
        return Q2_ERR_BUSY;
    q2_result_t e = authorized(m, target);
    if (e != Q2_OK)
        return e;
    if (target != m->current)
        m->pending = target;
    return Q2_OK;
}
static q2_result_t activate(q2_apps_t *m, int target) {
    q2_result_t e = Q2_OK;
    q2_app_status_t status = {0};
    if (m->runtimes[target])
        q2_lua_status(m->runtimes[target], &status);
    if (status.running) {
        m->current = target;
        Q2_LOGI(m->kernel, "apps", "foreground %s (resumed)", m->packages[target].meta.name);
        return Q2_OK;
    }
    if (!m->runtimes[target]) {
        e = q2_lua_create(m->kernel, &m->runtimes[target]);
        if (e != Q2_OK)
            return e;
    }
    if (m->prepare)
        e = m->prepare(m->context, m->packages[target].meta.id);
    if (e != Q2_OK)
        return e;
    package_t *p = &m->packages[target];
    m->current = target;
    e = q2_lua_start(m->runtimes[target], p->manifest, p->manifest_size, p->source, p->source_size, p->grants,
                     p->grant_count);
    if (e == Q2_OK)
        Q2_LOGI(m->kernel, "apps", "foreground %s", p->meta.name);
    return e;
}
q2_result_t q2_apps_advance(q2_apps_t *m, q2_result_t poll_result) {
    if (!m || !m->runtime || !m->ready)
        return Q2_ERR_STATE;
    /* A stale APP failure can share a result code with a new provider failure.
     * Never absorb the latter as a recoverable APP exit. */
    q2_kernel_status_t kernel_status;
    q2_kernel_status(m->kernel, &kernel_status);
    if (poll_result != Q2_OK && kernel_status.component_poll_result != Q2_OK)
        return kernel_status.component_poll_result;
    bool app_fault = false;
    for (unsigned i = 0; i < m->count; i++) {
        if (!m->runtimes[i])
            continue;
        q2_app_status_t status;
        q2_lua_status(m->runtimes[i], &status);
        if (status.running)
            continue;
        if (status.last_result != Q2_OK && status.last_result == poll_result)
            app_fault = true;
        q2_result_t e = q2_lua_stop(m->runtimes[i]);
        if (e != Q2_OK)
            return e;
        if ((int)i == m->home && m->current >= 0)
            return poll_result != Q2_OK ? poll_result : Q2_ERR_RUNTIME;
        if ((int)i == m->current) {
            snprintf(m->last_error, sizeof(m->last_error), "APP stopped: %.120s", status.error);
            m->pending = m->home;
        }
    }
    if (poll_result != Q2_OK && !app_fault)
        return poll_result;
    if (m->current < 0) {
        /* The resident runtime starts first, even when the product opens another initial APP. */
        q2_result_t e = activate(m, m->home);
        if (e != Q2_OK)
            return e;
    }
    int target = m->pending;
    m->pending = -1;
    if (target < 0 || target == m->current)
        return Q2_OK;
    if (target != m->home)
        m->last_error[0] = 0;
    q2_result_t e = activate(m, target);
    if (e == Q2_OK)
        return e;
    const char *name;
    q2_result_name(e, &name);
    snprintf(m->last_error, sizeof(m->last_error), "Cannot open %.63s: %s", m->packages[target].meta.name,
             name);
    Q2_LOGE(m->kernel, "apps", "%s", m->last_error);
    if (m->runtimes[target]) {
        q2_result_t cleanup = q2_lua_stop(m->runtimes[target]);
        if (cleanup != Q2_OK)
            return cleanup;
    }
    return activate(m, m->home);
}
q2_result_t q2_apps_state(q2_apps_t *m, uint64_t *home_instance, uint64_t *foreground_instance,
                          unsigned *running) {
    if (!m || !home_instance || !foreground_instance || !running)
        return Q2_ERR_ARGUMENT;
    *home_instance = *foreground_instance = 0;
    *running = 0;
    for (unsigned i = 0; i < m->count; i++)
        if (m->runtimes[i]) {
            q2_app_status_t status;
            q2_lua_status(m->runtimes[i], &status);
            if (status.running)
                (*running)++;
            if ((int)i == m->home)
                *home_instance = status.instance;
            if ((int)i == m->current)
                *foreground_instance = status.instance;
        }
    return Q2_OK;
}
q2_result_t q2_apps_shutdown(q2_apps_t *m) {
    if (!m)
        return Q2_ERR_ARGUMENT;
    /* Manager owns additional VMs; the original bound resident VM remains caller-owned. */
    for (unsigned i = 0; i < m->count; i++)
        if (m->runtimes[i] && (int)i != m->home) {
            q2_result_t e = q2_lua_destroy(&m->runtimes[i]);
            if (e != Q2_OK)
                return e;
        }
    return m->runtime ? q2_lua_stop(m->runtime) : Q2_OK;
}
q2_result_t q2_apps_destroy(q2_apps_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    q2_apps_t *m = *ref;
    if (m->ready)
        return Q2_ERR_BUSY;
    for (unsigned i = 0; i < m->count; i++) {
        free(m->packages[i].manifest);
        free(m->packages[i].source);
    }
    free(m);
    *ref = NULL;
    return Q2_OK;
}
