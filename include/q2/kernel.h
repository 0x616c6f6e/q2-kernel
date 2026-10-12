#ifndef Q2_KERNEL_H
#define Q2_KERNEL_H
#include "q2/port.h"
#include <stdbool.h>
#define Q2_COMPONENT_ABI 1u
#define Q2_COMPONENT_MAX 24u
#define Q2_DEPENDENCY_MAX 4u
#define Q2_EVENT_MAX 32u
#define Q2_RUNTIME_MAX 8u
#define Q2_CAPABILITY_MAX 32u
#define Q2_TEXT_MAX 256u
#define Q2_API_VERSION 2u
/** @file kernel.h All calls run on one owner/executor. IRQs and other threads
 * marshal through the port. */
typedef struct q2_kernel q2_kernel_t;
typedef enum { Q2_REGISTERED, Q2_STARTING, Q2_READY, Q2_FAILED, Q2_BLOCKED, Q2_STOPPED } q2_state_t;
typedef enum { Q2_NIL, Q2_BOOLEAN, Q2_INTEGER, Q2_STRING } q2_value_kind_t;
typedef struct {
    q2_value_kind_t kind;
    union {
        bool boolean;
        int64_t integer;
        char string[Q2_TEXT_MAX];
    } as;
} q2_value_t;
/** Trusted runtime-created caller. Never accept caller identity from Lua
 * arguments. */
typedef struct {
    char app_id[37];
    uint64_t instance;
    const char *grants[Q2_CAPABILITY_MAX];
    size_t grant_count;
} q2_caller_t;
typedef struct {
    char topic[32];
    uint64_t instance; /* 0: system-wide; otherwise only this runtime instance. */
    int64_t value;
    char text[128];
    /** Optional typed notification; legacy events keep has_data=false and value/text unchanged. */
    bool has_data, coalesce;
    char key[64];
    q2_value_t data;
    uint64_t subscription, sequence, timestamp_ms;
    q2_result_t data_status;
} q2_event_t;
/** Synchronous, bounded operation. out_value is valid only on Q2_OK; copy
 * retained input. */
typedef q2_result_t (*q2_method_fn)(q2_kernel_t *, void *, const q2_caller_t *, const q2_value_t *, size_t,
                                    q2_value_t *);
typedef struct {
    const char *module;
    const char *name;
    const char *capability;
    q2_method_fn invoke;
} q2_method_t;
typedef struct {
    const char *name;
    unsigned abi;
    size_t size;
    const void *api;
} q2_service_t;
/** Static descriptor, copied by reference. start must unwind its own partial
 * failure. stop/release_app must be idempotent and bounded; a failed stop
 * prevents destruction. */
typedef struct {
    unsigned abi;
    size_t size;
    const char *id;
    bool required;
    const char *needs[Q2_DEPENDENCY_MAX];
    const char *after[Q2_DEPENDENCY_MAX];
    void *context;
    q2_result_t (*start)(q2_kernel_t *, void *);
    q2_result_t (*stop)(q2_kernel_t *, void *);
    q2_result_t (*poll)(q2_kernel_t *, void *);
    q2_result_t (*release_app)(q2_kernel_t *, void *, uint64_t);
    const q2_service_t *services;
    size_t service_count;
    const q2_method_t *methods;
    size_t method_count;
    /** Additional READY capabilities for dynamically registered data contracts. Static lifetime. */
    const char *const *capabilities;
    size_t capability_count;
} q2_component_t;
/** Runtime attachment is independent of Lua; core does not include Lua headers.
 */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    q2_result_t (*event)(void *, const q2_event_t *);
    q2_result_t (*poll)(void *);
    q2_result_t (*stop)(void *);
} q2_runtime_driver_t;
typedef struct {
    const char *id;
    q2_state_t state;
    q2_result_t error;
} q2_component_status_t;
typedef struct {
    size_t components, queued;
    uint64_t posted, dropped;
    bool running;
    /** First component failure from the last completed poll, independent of APP runtime failures. */
    q2_result_t component_poll_result;
} q2_kernel_status_t;
/** Validate and copy the minimal port table; caller owns the resulting kernel.
 */
q2_result_t q2_kernel_create(const q2_port_t *port, q2_kernel_t **out_kernel);
/** Require detached runtime, then stop components in reverse order. Remains valid if cleanup
 * fails. */
q2_result_t q2_kernel_destroy(q2_kernel_t **kernel);
/** Register a static descriptor before boot. Duplicate IDs/services/methods are
 * rejected. */
q2_result_t q2_component_register(q2_kernel_t *kernel, const q2_component_t *component);
/** Validate the complete dependency graph before side effects; boot at most
 * once. */
q2_result_t q2_kernel_boot(q2_kernel_t *kernel);
/** Advance components/runtime and dispatch at most event_budget events; never
 * recursively call. */
q2_result_t q2_kernel_poll(q2_kernel_t *kernel, size_t event_budget);
/** Attach up to Q2_RUNTIME_MAX independent runtimes after boot. Duplicate contexts are rejected. */
q2_result_t q2_runtime_attach(q2_kernel_t *kernel, const q2_runtime_driver_t *driver);
/** Stop and detach the matching context; failure leaves attachment intact. */
q2_result_t q2_runtime_detach(q2_kernel_t *kernel, void *context);
/** Release resources for one nonzero application instance in reverse component
 * order. */
q2_result_t q2_kernel_release_app(q2_kernel_t *kernel, uint64_t instance);
/** Obtain a new, never reused application instance for this kernel lifetime. */
q2_result_t q2_kernel_next_instance(q2_kernel_t *kernel, uint64_t *out_instance);
/** Copy an event; Q2_ERR_LIMIT reports a full queue. Owner context only, never
 * ISR. */
q2_result_t q2_event_post(q2_kernel_t *kernel, const q2_event_t *event);
/** Cancel queued notifications for one instance/subscription. Owner only; no callbacks are invoked. */
q2_result_t q2_event_cancel(q2_kernel_t *kernel, uint64_t instance, uint64_t subscription);
/** Query immutable service table only from a READY provider; require ABI/size.
 */
q2_result_t q2_service_get(q2_kernel_t *kernel, const char *name, unsigned abi, size_t size,
                           const void **out_api);
/** Dispatch with shared authorization. The output is cleared even on error. */
q2_result_t q2_kernel_call(q2_kernel_t *kernel, const q2_caller_t *caller, const char *module,
                           const char *method, const q2_value_t *args, size_t count, q2_value_t *out_value);
/** Discover whether a registered READY component advertises a capability. */
q2_result_t q2_capability_available(q2_kernel_t *kernel, const char *name, bool *out_available);
/** Enumerate READY Lua methods; out-of-range returns Q2_ERR_NOT_FOUND. */
q2_result_t q2_method_at(q2_kernel_t *kernel, size_t index, const q2_method_t **out_method);
/** Read component state by registration index. */
q2_result_t q2_component_status(q2_kernel_t *kernel, size_t index, q2_component_status_t *out_status);
/** Copy bounded queue/lifecycle diagnostics. */
q2_result_t q2_kernel_status(q2_kernel_t *kernel, q2_kernel_status_t *out_status);
/** Read port monotonic time. */
q2_result_t q2_kernel_time(q2_kernel_t *kernel, uint64_t *out_ms);
/** Allocate through the port; core and Lua use this. Optional libraries may also
 * use the platform C
 * runtime heap; this is not a global heap quota. */
q2_result_t q2_kernel_allocate(q2_kernel_t *kernel, size_t size, void **out_memory);
/** Release through the port; NULL memory is accepted. */
q2_result_t q2_kernel_release(q2_kernel_t *kernel, void *memory);
#endif
