#ifndef Q2_AUDIO_H
#define Q2_AUDIO_H
#include "q2/kernel.h"
#define Q2_AUDIO_URL_MAX 2048u
/** Backend states are real transport/decoder states, never synthesized playback progress. */
typedef enum {
    Q2_AUDIO_IDLE,
    Q2_AUDIO_BUFFERING,
    Q2_AUDIO_PLAYING,
    Q2_AUDIO_PAUSED,
    Q2_AUDIO_ENDED,
    Q2_AUDIO_ERROR
} q2_audio_state_t;
typedef struct {
    q2_audio_state_t state;
    uint32_t position_ms, duration_ms;
    q2_result_t error;
} q2_audio_snapshot_t;
/** ABI 1. Owner-thread callbacks must return promptly; workers never call Kernel/Lua.
 * play copies the URL; stop cancels without joining. Platform owns worker resources and outlives component.
 * snapshot copies synchronized state. One physical player, exclusively owned by one APP instance. */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    q2_result_t (*play)(void *, const char *);
    q2_result_t (*pause)(void *, bool);
    q2_result_t (*stop)(void *);
    q2_result_t (*snapshot)(void *, q2_audio_snapshot_t *);
} q2_audio_backend_t;
typedef struct q2_audio q2_audio_t;
/** Copy validated backend; out remains NULL on failure. Register before boot. */
q2_result_t q2_audio_create(const q2_audio_backend_t *backend, q2_audio_t **out);
/** Borrow descriptor; audio must outlive its registration. */
q2_result_t q2_audio_component(q2_audio_t *audio, const q2_component_t **out);
/** Destroy after Kernel shutdown; NULL accepted, started component refused. */
q2_result_t q2_audio_destroy(q2_audio_t **audio);
#endif
