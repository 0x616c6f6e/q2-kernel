#include "q2/result.h"
#include <stddef.h>
q2_result_t q2_result_name(q2_result_t result, const char **out_name) {
    static const char *const names[] = {"ok",
                                        "invalid_argument",
                                        "out_of_memory",
                                        "invalid_state",
                                        "already_exists",
                                        "not_found",
                                        "missing_dependency",
                                        "dependency_cycle",
                                        "unsupported",
                                        "permission_denied",
                                        "busy",
                                        "quota_exceeded",
                                        "io_error",
                                        "timeout",
                                        "cancelled",
                                        "invalid_format",
                                        "runtime_error",
                                        "component_failed"};
    if (!out_name)
        return Q2_ERR_ARGUMENT;
    *out_name = (result <= 0 && result >= Q2_ERR_COMPONENT) ? names[-result] : "unknown_error";
    return Q2_OK;
}
