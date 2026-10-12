#ifndef Q2_LOG_H
#define Q2_LOG_H
#include "q2/kernel.h"
#ifndef Q2_ENABLE_LOG
#define Q2_ENABLE_LOG 1
#endif
#define Q2_LOG_ABI 1u
#define Q2_LOG_MESSAGE_MAX 192u
/** @file log.h Optional owner-thread logging. No global logger or mandatory SoC callback. */
typedef enum { Q2_LOG_OFF, Q2_LOG_ERROR, Q2_LOG_WARN, Q2_LOG_INFO, Q2_LOG_DEBUG } q2_log_level_t;
typedef struct {
    uint64_t timestamp_ms;
    q2_log_level_t level;
    char tag[32];
    char message[Q2_LOG_MESSAGE_MAX];
    bool truncated;
} q2_log_record_t;
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    /** Consume synchronously; record expires on return. Must not mutate/call back into Kernel.
     * Slow embedded transports should queue a bounded copy instead of blocking the owner. */
    q2_result_t (*write)(void *context, const q2_log_record_t *record);
} q2_log_sink_t;
/** Copy an optional sink and filter; NULL detaches. Owner thread only, not from a sink callback.
 * With Q2_ENABLE_LOG=0 returns Q2_ERR_UNSUPPORTED; a fresh kernel starts silent. */
q2_result_t q2_log_configure(q2_kernel_t *kernel, const q2_log_sink_t *sink, q2_log_level_t level);
/** Change filter without replacing the sink. OFF remains valid in a compiled-out build. */
q2_result_t q2_log_set_level(q2_kernel_t *kernel, q2_log_level_t level);
/** Return the current filter, or OFF when logging is compiled out. */
q2_result_t q2_log_get_level(q2_kernel_t *kernel, q2_log_level_t *out_level);
/** Query whether a record would be emitted; no sink, OFF or recursive emission returns false. */
q2_result_t q2_log_enabled(q2_kernel_t *kernel, q2_log_level_t level, bool *out_enabled);
/** Format at most 191 bytes plus NUL on the stack, mark truncation, and return sink errors.
 * Format must be a trusted C format string; pass untrusted text through "%s". Filtered writes succeed.
 * Recursive calls return BUSY; compiled-out calls return UNSUPPORTED. */
q2_result_t q2_log_write(q2_kernel_t *kernel, q2_log_level_t level, const char *tag, const char *format, ...);

/* Lazy formatting prevents side effects in discarded arguments. Diagnostics never replace business errors. */
#if Q2_ENABLE_LOG
#define Q2_LOG_AT(kernel, severity, tag, ...)                                                                \
    do {                                                                                                     \
        q2_kernel_t *q2_log_target_ = (kernel);                                                              \
        q2_log_level_t q2_log_severity_ = (severity);                                                        \
        bool q2_log_emit_ = false;                                                                           \
        if (q2_log_enabled(q2_log_target_, q2_log_severity_, &q2_log_emit_) == Q2_OK && q2_log_emit_)        \
            (void)q2_log_write(q2_log_target_, q2_log_severity_, (tag), __VA_ARGS__);                        \
    } while (0)
#else
#define Q2_LOG_AT(kernel, severity, tag, ...)                                                                \
    do {                                                                                                     \
    } while (0)
#endif
#define Q2_LOGE(kernel, tag, ...) Q2_LOG_AT(kernel, Q2_LOG_ERROR, tag, __VA_ARGS__)
#define Q2_LOGW(kernel, tag, ...) Q2_LOG_AT(kernel, Q2_LOG_WARN, tag, __VA_ARGS__)
#define Q2_LOGI(kernel, tag, ...) Q2_LOG_AT(kernel, Q2_LOG_INFO, tag, __VA_ARGS__)
#define Q2_LOGD(kernel, tag, ...) Q2_LOG_AT(kernel, Q2_LOG_DEBUG, tag, __VA_ARGS__)
#endif
