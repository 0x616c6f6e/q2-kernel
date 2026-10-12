#ifndef Q2_KEYS_H
#define Q2_KEYS_H
#include "q2/data.h"
#include "q2/kernel.h"
#define Q2_KEYS_MAX 32u
#define Q2_KEY_NAME_MAX 31u
typedef struct q2_keys q2_keys_t;
typedef enum { Q2_KEY_NONE = 0, Q2_KEY_SINGLE = 1, Q2_KEY_DOUBLE = 2, Q2_KEY_LONG = 3 } q2_key_action_t;
typedef struct {
    char name[Q2_KEY_NAME_MAX + 1];
    q2_key_action_t last_action;
} q2_key_state_t;
/** Create an optional named-key provider. All APIs use the Kernel owner, never ISR/worker threads. */
q2_result_t q2_keys_create(q2_kernel_t *kernel, q2_keys_t **out_keys);
/** Copy a unique name before boot: ASCII letter followed by letters/digits/_.-, maximum 31 bytes.
 * Initial last_action is NONE. No GPIO, active-level, gesture recognition or function mapping is stored here.
 */
q2_result_t q2_keys_register(q2_keys_t *keys, const char *name);
/** Register input.key.<NAME>.action event topics after names are registered, before boot.
 * Data receives only lifecycle-approved foreground actions; get never exposes raw input history.
 * Both objects remain alive until after Kernel shutdown. Only one binding is allowed. */
q2_result_t q2_keys_data(q2_keys_t *keys, q2_data_t *data);
/** Obtain the stable component descriptor; register before boot and retain keys until Kernel destruction. */
q2_result_t q2_keys_component(q2_keys_t *keys, const q2_component_t **out_component);
/** Forward a recognized SINGLE/DOUBLE/LONG action to the subscribed APP instance as key.action.
 * text=name, value=action (1/2/3). NONE/unknown actions are invalid. SoC/driver owns recognition;
 * Kernel never times clicks or maps their meaning. Repeated actions are not merged.
 * Provider diagnostic snapshot retains the latest raw action even on event-queue overflow; Q2_ERR_LIMIT
 * reports a dropped event. Managed Lua last() exposes only actions approved for that instance; C diagnostics
 * remain trusted. Without the data adapter/router, no subscriber means diagnostic snapshot update only. */
q2_result_t q2_keys_report(q2_keys_t *keys, const char *name, q2_key_action_t action);
/** Return registered key count, including before boot. Owner-thread inspection only. */
q2_result_t q2_keys_count(q2_keys_t *keys, size_t *out_count);
/** Copy a zero-based key snapshot; Q2_ERR_NOT_FOUND for out-of-range index. */
q2_result_t q2_keys_at(q2_keys_t *keys, size_t index, q2_key_state_t *out_state);
/** Free after Kernel destruction; refuses while the component is running. */
q2_result_t q2_keys_destroy(q2_keys_t **keys);
/** Set a trusted resident input router and current foreground. Only router may forward key events.
 * Owner thread only; use after APP transitions. This sets routing, never recognition or key behavior. */
q2_result_t q2_keys_route(q2_keys_t *keys, uint64_t lifecycle, uint64_t foreground);
#endif
