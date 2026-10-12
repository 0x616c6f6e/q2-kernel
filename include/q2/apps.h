#ifndef Q2_APPS_H
#define Q2_APPS_H
#include "q2/runtime.h"
#define Q2_APPS_MAX 8u
typedef struct q2_apps q2_apps_t;
/** Trusted product registration, not APP-controlled manifest privileges. Bytes/grants are copied. */
typedef struct {
    const char *manifest, *source;
    size_t manifest_size, source_size;
    const char *const *grants;
    size_t grant_count;
    bool factory, home;
} q2_app_package_t;
/** Prepare this package assets before its instance starts; other instances remain live, outside all Kernel
 * callbacks. Must return real failures; a failed target falls back to home once. NULL means no preparation
 * needed. */
typedef q2_result_t (*q2_app_prepare_fn)(void *context, const char *app_id);
/** Create an optional resident-home and multiple-VM application manager; register its component before boot.
 */
q2_result_t q2_apps_create(q2_kernel_t *kernel, q2_app_prepare_fn prepare, void *context, q2_apps_t **out);
/** Copy a validated package before boot. Exactly one home is required and it must be factory-owned. */
q2_result_t q2_apps_register(q2_apps_t *apps, const q2_app_package_t *package);
/** Get the stable component descriptor. Manager must outlive Kernel registration. */
q2_result_t q2_apps_component(q2_apps_t *apps, const q2_component_t **out);
/** Bind a runtime after boot; does not take ownership. Read saved system appearance if storage is ready. */
q2_result_t q2_apps_bind(q2_apps_t *apps, q2_lua_runtime_t *runtime);
/** Queue a known, authorized target from the owner. NULL selects home. A second pending request is BUSY. */
q2_result_t q2_apps_request(q2_apps_t *apps, const char *app_id);
/** Advance transitions OUTSIDE q2_kernel_poll and Lua callbacks, passing its actual result.
 * Preserves other running instances; normal APP exit/fault returns to the resident home.
 * A home failure or cleanup/component failure is fatal, never an infinite restart loop. */
q2_result_t q2_apps_advance(q2_apps_t *apps, q2_result_t poll_result);
/** Query resident/foreground instances and running VM count on the owner thread. */
q2_result_t q2_apps_state(q2_apps_t *apps, uint64_t *home_instance, uint64_t *foreground_instance,
                          unsigned *running);
/** Stop all APPs and destroy manager-owned VMs before caller destroys the bound runtime and Kernel.
 * Cleanup failures preserve ownership for retry. Call outside poll/callbacks. */
q2_result_t q2_apps_shutdown(q2_apps_t *apps);
/** Destroy after runtime and Kernel destruction; never dereferences the detached runtime. */
q2_result_t q2_apps_destroy(q2_apps_t **apps);
#endif
