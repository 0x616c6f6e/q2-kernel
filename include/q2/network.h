#ifndef Q2_NETWORK_H
#define Q2_NETWORK_H
#include "q2/kernel.h"
#define Q2_NETWORK_MAX 4u
#define Q2_NETWORK_BODY_MAX 262144u
#define Q2_NETWORK_UPLOAD_MAX 8192u
/** HTTP bytes borrowed until release, size <= BODY_MAX. Text requests additionally reject NUL. */
typedef struct {
    unsigned code;
    const char *body;
    size_t size;
} q2_network_response_t;
/** ABI 2 asynchronous transport. All callbacks are bounded owner calls.
 * start copies URL and returns an
 * owned job; poll returns BUSY until terminal result. release accepts NULL jobs, cancels/detaches without
 * waiting and clears job; backend retains worker resources until safe to reclaim. No worker may call
 * Kernel/Lua APIs. Backend outlives this component. */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    q2_result_t (*start)(void *, const char *, unsigned, void **);
    q2_result_t (*poll)(void *, void *, q2_network_response_t *);
    q2_result_t (*release)(void *, void **);
    /** Optional JSON POST: copy URL/body before returning; credentials must never be logged.
     * Body is
     * NUL-free, at most UPLOAD_MAX; no implicit cookies/authentication or redirects. */
    q2_result_t (*post)(void *, const char *, const char *, size_t, unsigned, void **);
} q2_network_backend_t;
/** Optional native service "network.binary", ABI 1. Call on the owner only.
 * Caller must hold network.http;
 * IDs are instance scoped. One binary request per instance
 * may coexist with its text request. read borrows
 * immutable bytes until close/APP release;
 * no pointer or raw binary crosses the Lua value boundary.
 * Transport limits/TLS rules are shared. */
typedef struct {
    void *context;
    q2_result_t (*get)(void *, const q2_caller_t *, const char *, unsigned, uint64_t *);
    q2_result_t (*read)(void *, const q2_caller_t *, uint64_t, q2_network_response_t *);
    q2_result_t (*close)(void *, const q2_caller_t *, uint64_t);
} q2_network_binary_t;
typedef struct q2_network q2_network_t;
/** Copy validated backend and create HTTP component on owner; out remains NULL on failure. */
q2_result_t q2_network_create(q2_kernel_t *kernel, const q2_network_backend_t *backend,
                              q2_network_t **out_network);
/** Borrow stable descriptor; network must outlive its Kernel registration. */
q2_result_t q2_network_component(q2_network_t *network, const q2_component_t **out_component);
/** Free after Kernel shutdown; refuses while started, NULL is accepted. */
q2_result_t q2_network_destroy(q2_network_t **network);
#endif
