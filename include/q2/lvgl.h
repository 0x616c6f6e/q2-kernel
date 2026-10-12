#ifndef Q2_LVGL_H
#define Q2_LVGL_H
#include "q2/kernel.h"

/** @file lvgl.h Optional LVGL display provider. No LVGL or window-system types cross this API. */
#define Q2_PIXEL_ABI 1u
#define Q2_UI_OBJECT_MAX 48u
#define Q2_UI_IMAGE_MAX 64u
#define Q2_UI_PNG_MAX (256u * 1024u)

typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    unsigned width, height;
    /** Copy a rectangle synchronously. Pixels are 32-bit 0x00RRGGBB, rows separated by stride bytes.
     * The pointer expires on return; asynchronous DMA must not retain it. Owner thread only. */
    q2_result_t (*flush)(void *context, unsigned x, unsigned y, unsigned width, unsigned height,
                         const uint32_t *pixels, size_t stride);
} q2_pixel_backend_t;

typedef struct q2_lvgl q2_lvgl_t;
/** Get the full logical display size in its configured orientation, in pixels.
 * Independent of APP viewport, status bar, scrolling or active layer; available before boot.
 * Returns Q2_ERR_ARGUMENT for null arguments. Called on the Kernel owner thread. */
q2_result_t q2_lvgl_resolution(const q2_lvgl_t *display, unsigned *width, unsigned *height);

/** Persistent system chrome. Configure before boot; APP coordinates exclude the status bar. */
typedef struct {
    bool status_bar, screensaver;
    unsigned status_bar_height;
    uint32_t idle_timeout_ms, clock_move_interval_ms;
} q2_system_ui_config_t;
/** Trusted platform status snapshot; battery_percent=-1 means unknown, never a simulated charge. */
typedef struct {
    bool clock_valid;
    unsigned hour, minute;
    int battery_percent;
    bool charging, wifi_connected, bluetooth_connected;
} q2_system_ui_status_t;
/** Physical display and APP viewport remain distinct while system chrome is enabled. */
typedef struct {
    bool screensaver_active;
    unsigned content_width, content_height, content_top;
    bool locked, lifecycle;
} q2_system_ui_state_t;
/** Product defaults copied before boot; preferences may subsequently be changed by trusted factory apps. */
typedef struct {
    bool lock_enabled;
    char status_left[128], status_right[128], saver_text[128], unlock_key[32];
    unsigned unlock_action;
} q2_lifecycle_config_t;
/** Enable Lua-owned system layers before boot. Trusted home UUID selects the resident owner.
 * Existing configure() supplies dimensions/timers; this removes native timer/render policy in managed mode.
 */
q2_result_t q2_lvgl_lifecycle(q2_lvgl_t *display, const char *home_id, const q2_lifecycle_config_t *defaults);
/** Permit a trusted factory UUID to modify persistent system preferences; call before boot. */
q2_result_t q2_lvgl_factory(q2_lvgl_t *display, const char *app_id);
/** Select the foreground APP instance on the owner thread. Other APP scenes are retained but hidden. */
q2_result_t q2_lvgl_focus(q2_lvgl_t *display, uint64_t instance);
/** Release package images only after every scene for its UUID has been released. Other APP images survive. */
q2_result_t q2_lvgl_clear_app_images(q2_lvgl_t *display, const char *app_id);
/** Scroll the current foreground vertically in physical pixels; wake-only and lock rules apply. */
q2_result_t q2_lvgl_scroll(q2_lvgl_t *display, int delta);
/** Set system chrome defaults before boot; lifecycle() optionally delegates policy to Lua.
 * Bar height 24..64 must leave at least 32 APP pixels; enabled timers must be positive. */
q2_result_t q2_lvgl_system_configure(q2_lvgl_t *display, const q2_system_ui_config_t *config);
/** Copy time/battery/link state from a trusted platform on the owner thread, before or after boot.
 * Time is local wall-clock supplied by the platform; idle timers use the Kernel monotonic clock. */
q2_result_t q2_lvgl_system_status(q2_lvgl_t *display, const q2_system_ui_status_t *status);
/** Report real user activity on the owner thread. A true out_consumed means this input belongs to system
 * wake/lock handling. An unlocked wake suppresses the complete gesture. While locked, send recognized keys to
 * the lifecycle router so Lua can match its unlock shortcut; never forward them directly to the foreground
 * APP. Pointer sources should use q2_lvgl_pointer, which suppresses the waking press/release automatically.
 */
q2_result_t q2_lvgl_activity(q2_lvgl_t *display, bool *out_consumed);
/** Snapshot system state on the owner thread; no internal LVGL pointers escape. */
q2_result_t q2_lvgl_system_state(q2_lvgl_t *display, q2_system_ui_state_t *out_state);
/** Allocate a portable LVGL provider, dimensions 64..1024. No window or LVGL global state starts yet.
 * kernel must remain valid through stop; destroy the provider after kernel destruction.
 * Only one running LVGL provider is supported per process. */
q2_result_t q2_lvgl_create(q2_kernel_t *kernel, const q2_pixel_backend_t *backend, q2_lvgl_t **out_display);
/** Select complete-frame rendering before boot on the owner thread; default is partial (32 rows).
 * Full mode renders width * height XRGB8888 pixels and calls the synchronous backend once per refresh.
 * It avoids narrow dirty-window transfers on rotated panels, at the cost of more RAM and repaint work.
 * Returns Q2_ERR_ARGUMENT for null, Q2_ERR_STATE while started, Q2_ERR_MEMORY if allocation fails.
 * Allocation failure preserves the previous buffer and mode. No display hardware is touched here. */
q2_result_t q2_lvgl_full_refresh(q2_lvgl_t *display, bool enabled);
/** Copy a trusted package PNG before boot or between fully released APP instances.
 * Scoped to an APP UUID and assets/<filename>.png; max 64 images, 256 KiB per file,
 * 512x512 pixels each and 2097152 total pixels.
 * Registration does not expose filesystem access to Lua; full PNG decoding occurs at boot or immediately if
 * running. Copied data lives until clear_images/destruction; decoded data until clear_images/stop. */
q2_result_t q2_lvgl_register_png(q2_lvgl_t *display, const char *app_id, const char *name, const void *bytes,
                                 size_t size);
/** Release all registered images only when no APP owns the display. Owner thread, outside poll.
 * Used during a prepared APP transition; refuses to invalidate active widgets. */
q2_result_t q2_lvgl_clear_images(q2_lvgl_t *display);
/** Obtain the provider's descriptor (ID "display"). Register instead of the legacy text provider.
 * The descriptor and backend context must outlive registration. */
q2_result_t q2_lvgl_component(q2_lvgl_t *display, const q2_component_t **out_component);
/** Supply a pointer transition from the owner thread, outside kernel callbacks. Coordinates are pixels.
 * Each transition is processed immediately so a press/release pair cannot disappear between polls. */
q2_result_t q2_lvgl_pointer(q2_lvgl_t *display, int x, int y, bool pressed);
/** Free only after kernel destruction. A running provider returns Q2_ERR_BUSY. */
q2_result_t q2_lvgl_destroy(q2_lvgl_t **display);
#endif
