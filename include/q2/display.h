#ifndef Q2_DISPLAY_H
#define Q2_DISPLAY_H
#include "q2/kernel.h"
/** @file display.h Minimal text display provider; a SoC can register any renderer without exposing its SDK.
 */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    unsigned width, height;
    q2_result_t (*clear)(void *context);
    q2_result_t (*text)(void *context, unsigned x, unsigned y, const char *utf8);
} q2_display_backend_t;
typedef struct q2_display q2_display_t;
/** Validate and copy a backend; callbacks run on the kernel owner, must be bounded. */
q2_result_t q2_display_create(const q2_display_backend_t *backend, q2_display_t **out_display);
/** Obtain a display component descriptor; display must outlive its registration. */
q2_result_t q2_display_component(q2_display_t *display, const q2_component_t **out_component);
/** Free after the kernel is destroyed; refuses while started. */
q2_result_t q2_display_destroy(q2_display_t **display);
#endif
