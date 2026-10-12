#ifndef Q2_RUNTIME_H
#define Q2_RUNTIME_H
#include "q2/kernel.h"
#define Q2_MANIFEST_MAX 4096u
#define Q2_SOURCE_MAX 32768u
#define Q2_MANIFEST_CAPS 16u
#define Q2_MANIFEST_RESOLUTIONS 16u
/** @file runtime.h Sandboxed Lua API 2; one isolated VM per runtime, up to Q2_RUNTIME_MAX attached VMs; no
 * SDK dependency. */
typedef struct {
    char id[37], name[64], version[64];
    char icon[97];
    /** Full logical WIDTHxHEIGHT display sizes; zero count means unspecified (legacy APP).
     * Informational metadata, not an automatic launch restriction. */
    char supported_resolutions[Q2_MANIFEST_RESOLUTIONS][10];
    size_t supported_resolution_count;
    char required[Q2_MANIFEST_CAPS][48], optional[Q2_MANIFEST_CAPS][48];
    size_t required_count, optional_count, heap_bytes;
} q2_manifest_t;
typedef struct q2_lua_runtime q2_lua_runtime_t;
typedef struct {
    bool running;
    size_t heap_bytes;
    uint64_t instance;
    q2_result_t last_result;
    char error[128];
} q2_app_status_t;
/** Validate bounded JSON, UUID v4, SemVer, API 2 and known configuration
 * fields. */
q2_result_t q2_manifest_parse(const char *json, size_t size, q2_manifest_t *out_manifest);
/** Attach a Lua runtime to a running kernel; no app starts until explicitly
 * loaded. */
q2_result_t q2_lua_create(q2_kernel_t *kernel, q2_lua_runtime_t **out_runtime);
/** Parse manifest, authorize declared capabilities and execute text-only
 * main.lua. grants is an explicit host decision; missing required grants
 * refuses launch. Callers must authenticate the package before passing bytes to
 * this API. Entry/on_start share an execution budget after bounded text compilation.
 * Registered component calls are excluded from elapsed Lua time, not from its instruction quota. */
q2_result_t q2_lua_start(q2_lua_runtime_t *runtime, const char *manifest, size_t manifest_size,
                         const char *source, size_t source_size, const char *const *grants,
                         size_t grant_count);
/** Stop VM and release instance-owned component resources; safe when already
 * stopped. */
q2_result_t q2_lua_stop(q2_lua_runtime_t *runtime);
/** Detach and free the runtime; a failed cleanup preserves it for retry. */
q2_result_t q2_lua_destroy(q2_lua_runtime_t **runtime);
/** Copy diagnostic state without exposing Lua pointers. */
q2_result_t q2_lua_status(q2_lua_runtime_t *runtime, q2_app_status_t *out_status);
#endif
