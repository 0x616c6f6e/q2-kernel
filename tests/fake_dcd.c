/* Test-only USB controller. Never link this file into a physical SoC build. */
#include "device/dcd.h"
#include "tusb.h"
#include <string.h>
unsigned q2_test_usb_transfers;
uint8_t q2_test_usb_last[256];
uint16_t q2_test_usb_length;
bool dcd_init(uint8_t p, const tusb_rhport_init_t *c) {
    (void)p;
    (void)c;
    return true;
}
bool dcd_deinit(uint8_t p) {
    (void)p;
    return true;
}
void dcd_int_handler(uint8_t p) {
    (void)p;
}
void dcd_int_enable(uint8_t p) {
    (void)p;
}
void dcd_int_disable(uint8_t p) {
    (void)p;
}
void dcd_set_address(uint8_t p, uint8_t a) {
    (void)p;
    (void)a;
}
void dcd_remote_wakeup(uint8_t p) {
    (void)p;
}
void dcd_connect(uint8_t p) {
    (void)p;
}
void dcd_disconnect(uint8_t p) {
    (void)p;
}
void dcd_sof_enable(uint8_t p, bool e) {
    (void)p;
    (void)e;
}
bool dcd_edpt_open(uint8_t p, const tusb_desc_endpoint_t *d) {
    (void)p;
    (void)d;
    return true;
}
void dcd_edpt_close_all(uint8_t p) {
    (void)p;
}
void dcd_edpt_close(uint8_t p, uint8_t e) {
    (void)p;
    (void)e;
}
bool dcd_edpt_xfer(uint8_t p, uint8_t ep, uint8_t *data, uint16_t n) {
    (void)p;
    (void)ep;
    q2_test_usb_transfers++;
    q2_test_usb_length = n;
    if (data && n <= sizeof(q2_test_usb_last))
        memcpy(q2_test_usb_last, data, n);
    return true;
}
void dcd_edpt_stall(uint8_t p, uint8_t e) {
    (void)p;
    (void)e;
}
void dcd_edpt_clear_stall(uint8_t p, uint8_t e) {
    (void)p;
    (void)e;
}
