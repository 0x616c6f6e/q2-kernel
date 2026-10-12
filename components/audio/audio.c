#include "q2/audio.h"
#include <stdlib.h>
#include <string.h>
struct q2_audio {
    q2_audio_backend_t backend;
    q2_component_t component;
    uint64_t owner;
    bool ready, draft;
    size_t length;
    char url[Q2_AUDIO_URL_MAX + 1];
};
static q2_result_t clear(q2_audio_t *s) {
    q2_result_t e = s->backend.stop(s->backend.context);
    if (e != Q2_OK)
        return e;
    s->owner = 0;
    s->length = 0;
    s->draft = false;
    memset(s->url, 0, sizeof(s->url));
    return Q2_OK;
}
static q2_result_t boot(q2_kernel_t *k, void *ctx) {
    (void)k;
    ((q2_audio_t *)ctx)->ready = true;
    return Q2_OK;
}
static q2_result_t shutdown(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_audio_t *s = ctx;
    q2_result_t e = clear(s);
    if (e == Q2_OK)
        s->ready = false;
    return e;
}
static q2_result_t release_app(q2_kernel_t *k, void *ctx, uint64_t owner) {
    (void)k;
    q2_audio_t *s = ctx;
    return s->owner == owner ? clear(s) : Q2_OK;
}
static q2_result_t open_url(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out) {
    (void)k;
    (void)a;
    (void)out;
    q2_audio_t *s = ctx;
    if (n || !c->instance)
        return Q2_ERR_ARGUMENT;
    if (s->owner && s->owner != c->instance)
        return Q2_ERR_BUSY;
    q2_result_t e = clear(s);
    if (e != Q2_OK)
        return e;
    s->owner = c->instance;
    s->draft = true;
    return Q2_OK;
}
static q2_result_t append(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                          q2_value_t *out) {
    (void)k;
    (void)out;
    q2_audio_t *s = ctx;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    if (!s->owner || s->owner != c->instance)
        return Q2_ERR_NOT_FOUND;
    if (!s->draft)
        return Q2_ERR_STATE;
    size_t size = strlen(a[0].as.string);
    if (size > Q2_AUDIO_URL_MAX - s->length)
        return Q2_ERR_LIMIT;
    memcpy(s->url + s->length, a[0].as.string, size + 1);
    s->length += size;
    return Q2_OK;
}
static bool valid(const char *url) {
    const char *p = !strncmp(url, "https://", 8) ? url + 8 : !strncmp(url, "http://", 7) ? url + 7 : NULL;
    if (!p || !*p || *p == '/' || *p == '?')
        return false;
    for (const char *x = p; *x; x++) {
        if ((unsigned char)*x <= 32 || *x == '\\' || *x == '#')
            return false;
        if (*x == '@' && x < p + strcspn(p, "/?"))
            return false;
    }
    return true;
}
static q2_result_t play(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                        q2_value_t *out) {
    (void)k;
    (void)a;
    (void)out;
    q2_audio_t *s = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (!s->owner || s->owner != c->instance)
        return Q2_ERR_NOT_FOUND;
    if (!s->draft)
        return Q2_ERR_STATE;
    /* A playback grant does not implicitly authorize network access. */
    bool network = false;
    for (size_t i = 0; i < c->grant_count; i++)
        if (!strcmp(c->grants[i], "network.http"))
            network = true;
    if (!network)
        return Q2_ERR_PERMISSION;
    if (!valid(s->url))
        return Q2_ERR_ARGUMENT;
    q2_result_t e = s->backend.play(s->backend.context, s->url);
    if (e == Q2_OK) {
        s->draft = false;
        memset(s->url, 0, sizeof(s->url));
        s->length = 0;
    }
    return e;
}
static q2_result_t pause_audio(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                               q2_value_t *out) {
    (void)k;
    (void)out;
    q2_audio_t *s = ctx;
    if (n != 1 || a[0].kind != Q2_BOOLEAN)
        return Q2_ERR_ARGUMENT;
    if (!s->owner || s->owner != c->instance)
        return Q2_ERR_NOT_FOUND;
    if (s->draft)
        return Q2_ERR_STATE;
    return s->backend.pause(s->backend.context, a[0].as.boolean);
}
static q2_result_t stop_audio(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *out) {
    (void)k;
    (void)a;
    (void)out;
    q2_audio_t *s = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (!s->owner)
        return Q2_OK;
    return s->owner == c->instance ? clear(s) : Q2_ERR_NOT_FOUND;
}
static q2_result_t read_snapshot(void *ctx, const q2_caller_t *c, size_t n, q2_audio_snapshot_t *snapshot) {
    q2_audio_t *s = ctx;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (!s->owner || s->owner != c->instance)
        return Q2_ERR_NOT_FOUND;
    if (s->draft) {
        *snapshot = (q2_audio_snapshot_t){.state = Q2_AUDIO_IDLE};
        return Q2_OK;
    }
    q2_result_t e = s->backend.snapshot(s->backend.context, snapshot);
    if (e == Q2_OK && snapshot->state == Q2_AUDIO_ERROR)
        return snapshot->error == Q2_OK ? Q2_ERR_IO : snapshot->error;
    if (e == Q2_OK && (unsigned)snapshot->state > Q2_AUDIO_ERROR)
        return Q2_ERR_STATE;
    return e;
}
static q2_result_t state(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                         q2_value_t *out) {
    (void)k;
    (void)a;
    q2_audio_snapshot_t info;
    q2_result_t e = read_snapshot(ctx, c, n, &info);
    if (e != Q2_OK)
        return e;
    const char *names[] = {"idle", "buffering", "playing", "paused", "ended", "error"};
    out->kind = Q2_STRING;
    strcpy(out->as.string, names[info.state]);
    return Q2_OK;
}
static q2_result_t position(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out) {
    (void)k;
    (void)a;
    q2_audio_snapshot_t info;
    q2_result_t e = read_snapshot(ctx, c, n, &info);
    if (e == Q2_OK) {
        out->kind = Q2_INTEGER;
        out->as.integer = info.position_ms;
    }
    return e;
}
static q2_result_t duration(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out) {
    (void)k;
    (void)a;
    q2_audio_snapshot_t info;
    q2_result_t e = read_snapshot(ctx, c, n, &info);
    if (e == Q2_OK) {
        out->kind = Q2_INTEGER;
        out->as.integer = info.duration_ms;
    }
    return e;
}
static const q2_method_t methods[] = {
    {"audio", "open", "audio.playback", open_url},     {"audio", "write", "audio.playback", append},
    {"audio", "play", "audio.playback", play},         {"audio", "pause", "audio.playback", pause_audio},
    {"audio", "stop", "audio.playback", stop_audio},   {"audio", "state", "audio.playback", state},
    {"audio", "position", "audio.playback", position}, {"audio", "duration", "audio.playback", duration}};
q2_result_t q2_audio_create(const q2_audio_backend_t *b, q2_audio_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!b || b->abi != 1 || b->size != sizeof(*b) || !b->play || !b->pause || !b->stop || !b->snapshot)
        return Q2_ERR_ARGUMENT;
    q2_audio_t *s = calloc(1, sizeof(*s));
    if (!s)
        return Q2_ERR_MEMORY;
    s->backend = *b;
    s->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = "audio",
                                    .context = s,
                                    .start = boot,
                                    .stop = shutdown,
                                    .release_app = release_app,
                                    .methods = methods,
                                    .method_count = sizeof(methods) / sizeof(*methods)};
    *out = s;
    return Q2_OK;
}
q2_result_t q2_audio_component(q2_audio_t *s, const q2_component_t **out) {
    if (!s || !out)
        return Q2_ERR_ARGUMENT;
    *out = &s->component;
    return Q2_OK;
}
q2_result_t q2_audio_destroy(q2_audio_t **s) {
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
