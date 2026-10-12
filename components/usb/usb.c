#include "q2/usb.h"
#include "tusb.h"
#include <stdlib.h>
#include <string.h>
struct q2_usb {
    q2_kernel_t *kernel;
    q2_usb_config_t config;
    q2_component_t component;
    bool started;
    tusb_desc_device_t descriptor;
};
static q2_usb_t *owner;
uint32_t tusb_time_millis_api(void) {
    uint64_t now = 0;
    if (owner)
        q2_kernel_time(owner->kernel, &now);
    return (uint32_t)now;
}
uint8_t const *tud_descriptor_device_cb(void) {
    return owner ? (const uint8_t *)&owner->descriptor : NULL;
}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    static const uint8_t config[] = {
        TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
        TUD_CDC_DESCRIPTOR(0, 0, 0x81, 8, 0x02, 0x82, 64)};
    return index == 0 ? config : NULL;
}
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language) {
    (void)language;
    static uint16_t buffer[32];
    const char *strings[] = {"", "Q2-Kernel", "Q2 CDC"};
    if (index == 0) {
        buffer[0] = (TUSB_DESC_STRING << 8) | 4;
        buffer[1] = 0x0409;
        return buffer;
    }
    if (index >= 3)
        return NULL;
    size_t n = strlen(strings[index]);
    buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    for (size_t i = 0; i < n; i++)
        buffer[i + 1] = (uint8_t)strings[index][i];
    return buffer;
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_usb_t *u = ctx;
    if (owner)
        return Q2_ERR_BUSY;
    owner = u;
    u->kernel = k;
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    if (!tusb_init(u->config.root_port, &init)) {
        owner = NULL;
        return Q2_ERR_IO;
    }
    u->started = true;
    return Q2_OK;
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_usb_t *u = ctx;
    if (!u->started)
        return Q2_OK;
    if (!tud_deinit(u->config.root_port))
        return Q2_ERR_IO;
    u->started = false;
    owner = NULL;
    return Q2_OK;
}
static q2_result_t poll(q2_kernel_t *k, void *ctx) {
    (void)k;
    (void)ctx;
    tud_task_ext(0, false);
    return Q2_OK;
}
static q2_result_t connected(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *o) {
    (void)k;
    (void)ctx;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    o->kind = Q2_BOOLEAN;
    o->as.boolean = tud_cdc_connected();
    return Q2_OK;
}
static q2_result_t write_text(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                              q2_value_t *o) {
    (void)k;
    (void)ctx;
    (void)c;
    if (n != 1 || a[0].kind != Q2_STRING)
        return Q2_ERR_ARGUMENT;
    if (!tud_cdc_connected())
        return Q2_ERR_STATE;
    size_t bytes = strlen(a[0].as.string);
    if (tud_cdc_write_available() < bytes)
        return Q2_ERR_BUSY;
    o->kind = Q2_INTEGER;
    o->as.integer = tud_cdc_write(a[0].as.string, (uint32_t)bytes);
    tud_cdc_write_flush();
    return Q2_OK;
}
static q2_result_t read_text(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *o) {
    (void)k;
    (void)ctx;
    (void)c;
    (void)a;
    if (n)
        return Q2_ERR_ARGUMENT;
    if (!tud_cdc_connected())
        return Q2_ERR_STATE;
    uint32_t bytes = tud_cdc_read(o->as.string, Q2_TEXT_MAX - 1);
    if (memchr(o->as.string, 0, bytes))
        return Q2_ERR_FORMAT;
    o->as.string[bytes] = 0;
    o->kind = Q2_STRING;
    return Q2_OK;
}
static const q2_method_t methods[] = {{"usb", "connected", "usb.cdc", connected},
                                      {"usb", "write_text", "usb.cdc", write_text},
                                      {"usb", "read_text", "usb.cdc", read_text}};
q2_result_t q2_usb_create(const q2_usb_config_t *c, q2_usb_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!c || !c->vendor_id || !c->product_id || c->root_port)
        return Q2_ERR_ARGUMENT;
    q2_usb_t *u = calloc(1, sizeof(*u));
    if (!u)
        return Q2_ERR_MEMORY;
    u->config = *c;
    u->descriptor = (tusb_desc_device_t){.bLength = sizeof(tusb_desc_device_t),
                                         .bDescriptorType = TUSB_DESC_DEVICE,
                                         .bcdUSB = 0x0200,
                                         .bDeviceClass = TUSB_CLASS_MISC,
                                         .bDeviceSubClass = MISC_SUBCLASS_COMMON,
                                         .bDeviceProtocol = MISC_PROTOCOL_IAD,
                                         .bMaxPacketSize0 = 64,
                                         .idVendor = c->vendor_id,
                                         .idProduct = c->product_id,
                                         .bcdDevice = 0x0100,
                                         .iManufacturer = 1,
                                         .iProduct = 2,
                                         .bNumConfigurations = 1};
    u->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = "usb",
                                    .context = u,
                                    .start = start,
                                    .stop = stop,
                                    .poll = poll,
                                    .methods = methods,
                                    .method_count = 3};
    *out = u;
    return Q2_OK;
}
q2_result_t q2_usb_component(q2_usb_t *u, const q2_component_t **out) {
    if (!u || !out)
        return Q2_ERR_ARGUMENT;
    *out = &u->component;
    return Q2_OK;
}
q2_result_t q2_usb_destroy(q2_usb_t **u) {
    if (!u || !*u)
        return Q2_ERR_ARGUMENT;
    if ((*u)->started)
        return Q2_ERR_BUSY;
    free(*u);
    *u = NULL;
    return Q2_OK;
}
