#ifndef Q2_DATA_H
#define Q2_DATA_H
#include "q2/kernel.h"
/** @file data.h Optional typed state/event registry. All calls use the Kernel owner, never ISR.
 * Providers own sampling; get is cached, publish copies data. No Lua write access is exported. */
#define Q2_DATA_MAX 64u
#define Q2_DATA_SUB_MAX 128u
#define Q2_DATA_APP_SUB_MAX 16u
typedef struct q2_data q2_data_t;
typedef enum { Q2_DATA_STATE, Q2_DATA_EVENT } q2_data_kind_t;
typedef struct {
    const char *name, *capability, *unit;
    q2_data_kind_t kind;
    q2_value_kind_t type;
    bool bounded, foreground_only;
    int64_t minimum, maximum;
    uint32_t ttl_ms;
} q2_data_spec_t;
/** Allocate an optional registry. Does not register or boot it; destroy after Kernel shutdown. */
q2_result_t q2_data_create(q2_kernel_t *kernel, q2_data_t **out_data);
/** Copy a unique descriptor before boot. Names 1..63 ASCII letters/digits/_.-; cap <=47, unit <=15.
 * owner is a non-NULL trusted publisher identity; use returned handle only with that owner.
 * Bool/int/string only. Foreground-only contracts must be events; ttl applies only to states. */
q2_result_t q2_data_register(q2_data_t *data, const void *owner, const q2_data_spec_t *spec,
                             unsigned *out_handle);
/** Obtain the stable data component descriptor. Finish registration before Kernel component_register. */
q2_result_t q2_data_component(q2_data_t *data, const q2_component_t **out_component);
/** Publish a copied sample from the registered owner after boot. Non-OK status invalidates state.
 * Unchanged state refreshes age without notifying. Events always notify, even equal values.
 * Event overflow returns LIMIT (partial fanout possible); state notifications retry/coalesce on poll. */
q2_result_t q2_data_publish(q2_data_t *data, const void *owner, unsigned handle, const q2_value_t *value,
                            q2_result_t status);
/** Update allowed foreground instance; cancels queued foreground-only notifications for old recipients.
 * Zero disables delivery; subscriptions survive hiding. Must be set before accepting input. */
q2_result_t q2_data_focus(q2_data_t *data, uint64_t instance);
/** Free after Kernel destruction; a running registry returns BUSY. */
q2_result_t q2_data_destroy(q2_data_t **data);
#endif
