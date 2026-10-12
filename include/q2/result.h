#ifndef Q2_RESULT_H
#define Q2_RESULT_H
/** @file result.h Shared operation result. Zero is success; failures never
 * return data as success. */
typedef enum {
    Q2_OK = 0,
    Q2_ERR_ARGUMENT = -1,
    Q2_ERR_MEMORY = -2,
    Q2_ERR_STATE = -3,
    Q2_ERR_EXISTS = -4,
    Q2_ERR_NOT_FOUND = -5,
    Q2_ERR_DEPENDENCY = -6,
    Q2_ERR_CYCLE = -7,
    Q2_ERR_UNSUPPORTED = -8,
    Q2_ERR_PERMISSION = -9,
    Q2_ERR_BUSY = -10,
    Q2_ERR_LIMIT = -11,
    Q2_ERR_IO = -12,
    Q2_ERR_TIMEOUT = -13,
    Q2_ERR_CANCELLED = -14,
    Q2_ERR_FORMAT = -15,
    Q2_ERR_RUNTIME = -16,
    Q2_ERR_COMPONENT = -17
} q2_result_t;
/** Return a static diagnostic name through out_name. NULL output is invalid. */
q2_result_t q2_result_name(q2_result_t result, const char **out_name);
#endif
