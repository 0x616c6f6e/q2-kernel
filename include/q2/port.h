#ifndef Q2_PORT_H
#define Q2_PORT_H
#include "q2/result.h"
#include <stddef.h>
#include <stdint.h>
#define Q2_PORT_ABI 1u
/** @file port.h Minimal SoC contract; no OS, GPIO, SDK, thread or filesystem
 * types. */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    /** Allocate at least size bytes, aligned for max_align_t. NULL means failure.
     */
    void *(*allocate)(void *context, size_t size);
    /** Release a previous allocation; NULL must be accepted. */
    void (*release)(void *context, void *memory);
    /** Return monotonic milliseconds. The value must not go backwards. */
    uint64_t (*now_ms)(void *context);
} q2_port_t;
#endif
