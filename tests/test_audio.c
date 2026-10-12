#include "q2/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, stops, plays;
static bool paused;
static q2_audio_snapshot_t current;
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
    return 0;
}
static q2_result_t play(void *c, const char *url) {
    (void)c;
    (void)url;
    plays++;
    current.state = Q2_AUDIO_BUFFERING;
    return Q2_OK;
}
static q2_result_t pause_audio(void *c, bool value) {
    (void)c;
    paused = value;
    return Q2_OK;
}
static q2_result_t stop(void *c) {
    (void)c;
    stops++;
    return Q2_OK;
}
static q2_result_t snapshot(void *c, q2_audio_snapshot_t *out) {
    (void)c;
    *out = current;
    return Q2_OK;
}
static q2_value_t str(const char *text) {
    q2_value_t v = {.kind = Q2_STRING};
    strcpy(v.as.string, text);
    return v;
}
int main(void) {
    q2_port_t port = {1, sizeof(port), NULL, allocate, release, now};
    q2_audio_backend_t backend = {1, sizeof(backend), NULL, play, pause_audio, stop, snapshot};
    q2_kernel_t *k = NULL;
    q2_audio_t *audio = NULL;
    const q2_component_t *component;
    q2_caller_t user = {.instance = 1, .grants = {"audio.playback", "network.http"}, .grant_count = 2},
                other = user;
    other.instance = 2;
    q2_value_t out, a;
    CHECK(q2_kernel_create(&port, &k) == Q2_OK);
    CHECK(q2_audio_create(&backend, &audio) == Q2_OK);
    CHECK(q2_audio_component(audio, &component) == Q2_OK);
    CHECK(q2_component_register(k, component) == Q2_OK);
    CHECK(q2_kernel_boot(k) == Q2_OK);
    CHECK(q2_audio_destroy(&audio) == Q2_ERR_STATE);
    other.grant_count = 0;
    CHECK(q2_kernel_call(k, &other, "audio", "open", NULL, 0, &out) == Q2_ERR_PERMISSION);
    CHECK(stops == 0);
    other.grant_count = 2;
    CHECK(q2_kernel_call(k, &user, "audio", "open", NULL, 0, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &other, "audio", "open", NULL, 0, &out) == Q2_ERR_BUSY);
    CHECK(q2_kernel_call(k, &other, "audio", "stop", NULL, 0, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &other, "audio", "state", NULL, 0, &out) == Q2_ERR_NOT_FOUND);
    CHECK(q2_kernel_call(k, &user, "audio", "play", NULL, 0, &out) == Q2_ERR_ARGUMENT);
    const char *bad[] = {"file:///secret.mp3", "https://u:p@host/", "https://x/ a", "http://x/#a", "http://"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        CHECK(q2_kernel_call(k, &user, "audio", "open", NULL, 0, &out) == Q2_OK);
        a = str(bad[i]);
        CHECK(q2_kernel_call(k, &user, "audio", "write", &a, 1, &out) == Q2_OK);
        CHECK(q2_kernel_call(k, &user, "audio", "play", NULL, 0, &out) == Q2_ERR_ARGUMENT);
    }
    CHECK(plays == 0);
    CHECK(q2_kernel_call(k, &user, "audio", "open", NULL, 0, &out) == Q2_OK);
    a = str("https://example.invalid/audio.mp3");
    CHECK(q2_kernel_call(k, &user, "audio", "write", &a, 1, &out) == Q2_OK);
    user.grant_count = 1;
    CHECK(q2_kernel_call(k, &user, "audio", "play", NULL, 0, &out) == Q2_ERR_PERMISSION);
    user.grant_count = 2;
    CHECK(q2_kernel_call(k, &user, "audio", "play", NULL, 0, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &user, "audio", "write", &a, 1, &out) == Q2_ERR_STATE);
    CHECK(q2_kernel_call(k, &user, "audio", "state", NULL, 0, &out) == Q2_OK &&
          !strcmp(out.as.string, "buffering"));
    current = (q2_audio_snapshot_t){Q2_AUDIO_PLAYING, 1234, 5000, Q2_OK};
    CHECK(q2_kernel_call(k, &user, "audio", "position", NULL, 0, &out) == Q2_OK && out.as.integer == 1234);
    CHECK(q2_kernel_call(k, &user, "audio", "duration", NULL, 0, &out) == Q2_OK && out.as.integer == 5000);
    a = (q2_value_t){.kind = Q2_BOOLEAN, .as.boolean = true};
    CHECK(q2_kernel_call(k, &user, "audio", "pause", &a, 1, &out) == Q2_OK && paused);
    current.state = Q2_AUDIO_ERROR;
    current.error = Q2_ERR_FORMAT;
    CHECK(q2_kernel_call(k, &user, "audio", "state", NULL, 0, &out) == Q2_ERR_FORMAT);
    CHECK(q2_kernel_release_app(k, 1) == Q2_OK);
    CHECK(q2_kernel_call(k, &other, "audio", "open", NULL, 0, &out) == Q2_OK);
    memset(a.as.string, 'a', 255);
    a.as.string[255] = 0;
    a.kind = Q2_STRING;
    for (int i = 0; i < 8; i++)
        CHECK(q2_kernel_call(k, &other, "audio", "write", &a, 1, &out) == Q2_OK);
    CHECK(q2_kernel_call(k, &other, "audio", "write", &a, 1, &out) == Q2_ERR_LIMIT);
    CHECK(q2_kernel_destroy(&k) == Q2_OK);
    CHECK(q2_audio_destroy(&audio) == Q2_OK);
    printf("PASS: %u audio ownership/permission/state checks\n", checks);
    return 0;
}
