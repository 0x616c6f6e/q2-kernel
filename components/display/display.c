#include "q2/display.h"
#include <stdlib.h>
struct q2_display {
    q2_display_backend_t backend;
    q2_component_t component;
    bool started;
    uint64_t owner;
};
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_display_t *d = ctx;
    q2_result_t e = d->backend.clear(d->backend.context);
    d->started = e == Q2_OK;
    return e;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t instance) {
    (void)k;
    q2_display_t *d = ctx;
    if (d->owner != instance)
        return Q2_OK;
    q2_result_t e = d->backend.clear(d->backend.context);
    if (e == Q2_OK)
        d->owner = 0;
    return e;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    q2_display_t *d = ctx;
    q2_result_t e = release_app(k, ctx, d->owner);
    if (e == Q2_OK)
        d->started = false;
    return e;
}
static q2_result_t text(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)out;
    q2_display_t *d = ctx;
    if (n != 3 || a[0].kind != Q2_INTEGER || a[1].kind != Q2_INTEGER || a[2].kind != Q2_STRING ||
        a[0].as.integer < 0 || a[1].as.integer < 0 || (uint64_t)a[0].as.integer >= d->backend.width ||
        (uint64_t)a[1].as.integer >= d->backend.height)
        return Q2_ERR_ARGUMENT;
    if (d->owner && d->owner != c->instance)
        return Q2_ERR_BUSY;
    d->owner = c->instance;
    return d->backend.text(d->backend.context, (unsigned)a[0].as.integer, (unsigned)a[1].as.integer,
                           a[2].as.string);
}
static q2_result_t clear(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)a;
    (void)out;
    q2_display_t *d = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (d->owner && d->owner != c->instance)
        return Q2_ERR_BUSY;
    d->owner = c->instance;
    return d->backend.clear(d->backend.context);
}
static const q2_method_t methods[] = {{"display", "text", "display.text", text},
                                      {"display", "clear", "display.text", clear}};
q2_result_t q2_display_create(const q2_display_backend_t *b, q2_display_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!b || b->abi != 1 || b->size < sizeof(*b) || !b->width || !b->height || !b->clear || !b->text)
        return Q2_ERR_ARGUMENT;
    q2_display_t *d = calloc(1, sizeof(*d));
    if (!d)
        return Q2_ERR_MEMORY;
    d->backend = *b;
    d->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = "display",
                                    .context = d,
                                    .start = start,
                                    .stop = stop,
                                    .release_app = release_app,
                                    .methods = methods,
                                    .method_count = 2};
    *out = d;
    return Q2_OK;
}
q2_result_t q2_display_component(q2_display_t *d, const q2_component_t **out) {
    if (!d || !out)
        return Q2_ERR_ARGUMENT;
    *out = &d->component;
    return Q2_OK;
}
q2_result_t q2_display_destroy(q2_display_t **d) {
    if (!d || !*d)
        return Q2_ERR_ARGUMENT;
    if ((*d)->started)
        return Q2_ERR_BUSY;
    free(*d);
    *d = NULL;
    return Q2_OK;
}
