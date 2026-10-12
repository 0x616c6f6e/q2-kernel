#ifndef Q2_USB_H
#define Q2_USB_H
#include "q2/kernel.h"
/** @file usb.h TinyUSB CDC device component. The SoC supplies TinyUSB DCD and
 * IRQ routing. */
typedef struct q2_usb q2_usb_t;
typedef struct {
    uint16_t vendor_id, product_id;
    uint8_t root_port;
} q2_usb_config_t;
/** Allocate a CDC component. Caller must use legitimately assigned VID/PID on
 * real hardware. */
q2_result_t q2_usb_create(const q2_usb_config_t *config, q2_usb_t **out_usb);
/** Return owned descriptor; requires Q2_WITH_USB and a linked DCD. */
q2_result_t q2_usb_component(q2_usb_t *usb, const q2_component_t **out_component);
/** Destroy after kernel stop. No automatic stop from another task/ISR. */
q2_result_t q2_usb_destroy(q2_usb_t **usb);
#endif
