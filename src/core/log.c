#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static bool valid_level(q2_log_level_t level) {
    return level >= Q2_LOG_OFF && level <= Q2_LOG_DEBUG;
}
q2_result_t q2_log_configure(q2_kernel_t *k, const q2_log_sink_t *sink, q2_log_level_t level) {
    if (!k || !valid_level(level) ||
        (sink && (sink->abi != Q2_LOG_ABI || sink->size < sizeof(*sink) || !sink->write)))
        return Q2_ERR_ARGUMENT;
#if Q2_ENABLE_LOG
    if (k->logging)
        return Q2_ERR_BUSY;
    if (sink)
        k->log_sink = *sink;
    else
        memset(&k->log_sink, 0, sizeof(k->log_sink));
    k->log_level = level;
    return Q2_OK;
#else
    return Q2_ERR_UNSUPPORTED;
#endif
}
q2_result_t q2_log_set_level(q2_kernel_t *k, q2_log_level_t level) {
    if (!k || !valid_level(level))
        return Q2_ERR_ARGUMENT;
#if Q2_ENABLE_LOG
    if (k->logging)
        return Q2_ERR_BUSY;
    k->log_level = level;
    return Q2_OK;
#else
    return level == Q2_LOG_OFF ? Q2_OK : Q2_ERR_UNSUPPORTED;
#endif
}
q2_result_t q2_log_get_level(q2_kernel_t *k, q2_log_level_t *out) {
    if (!k || !out)
        return Q2_ERR_ARGUMENT;
    *out = Q2_LOG_OFF;
#if Q2_ENABLE_LOG
    *out = k->log_level;
#endif
    return Q2_OK;
}
q2_result_t q2_log_enabled(q2_kernel_t *k, q2_log_level_t level, bool *out) {
    if (!k || !out || !valid_level(level) || level == Q2_LOG_OFF)
        return Q2_ERR_ARGUMENT;
    *out = false;
#if Q2_ENABLE_LOG
    *out = k->log_sink.write && !k->logging && level <= k->log_level;
#endif
    return Q2_OK;
}
q2_result_t q2_log_write(q2_kernel_t *k, q2_log_level_t level, const char *tag, const char *format, ...) {
    if (!k || !valid_level(level) || level == Q2_LOG_OFF || !tag || !*tag || strlen(tag) >= 32 || !format)
        return Q2_ERR_ARGUMENT;
#if Q2_ENABLE_LOG
    if (k->logging)
        return Q2_ERR_BUSY;
    if (!k->log_sink.write || level > k->log_level)
        return Q2_OK;
    q2_log_record_t record = {.level = level};
    strcpy(record.tag, tag);
    va_list args;
    va_start(args, format);
    int count = vsnprintf(record.message, sizeof(record.message), format, args);
    va_end(args);
    if (count < 0)
        return Q2_ERR_FORMAT;
    record.truncated = (size_t)count >= sizeof(record.message);
    /* Guard both clock and sink callbacks against recursive diagnostic emission. */
    k->logging = true;
    record.timestamp_ms = k->port.now_ms(k->port.context);
    q2_result_t e = k->log_sink.write(k->log_sink.context, &record);
    k->logging = false;
    return e;
#else
    return Q2_ERR_UNSUPPORTED;
#endif
}
