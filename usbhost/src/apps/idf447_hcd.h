// ESP-IDF v4.4.7 USB Host Controller Driver (HCD) API, vendored.
//
// arduino-esp32 2.0.17 links IDF 4.4.7's libusb.a (hcd_*, urb_alloc are in it)
// but doesn't ship the private headers. These declarations are copied from
// esp-idf v4.4.7 components/usb/private_include/{hcd.h,usb_private.h} and MUST
// match that exact version: do not use with another IDF.
// Source: https://github.com/espressif/esp-idf/tree/v4.4.7/components/usb
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>
#include "esp_err.h"
#include "esp_idf_version.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#if !(ESP_IDF_VERSION_MAJOR == 4 && ESP_IDF_VERSION_MINOR == 4)
#error "idf447_hcd.h only matches ESP-IDF 4.4.x (arduino-esp32 2.0.x)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- usb_private.h ----
struct urb_s {
    TAILQ_ENTRY(urb_s) tailq_entry;
    void *hcd_ptr;
    uint32_t hcd_var;
    void *usb_host_client;
    size_t usb_host_header_size;
    bool usb_host_inflight;
    usb_transfer_t transfer;        // must be last (variable length array)
};
typedef struct urb_s urb_t;

urb_t *urb_alloc(size_t data_buffer_size, size_t header_size, int num_isoc_packets);
void urb_free(urb_t *urb);

// ---- hcd.h ----
typedef enum {
    HCD_PORT_STATE_NOT_POWERED,
    HCD_PORT_STATE_DISCONNECTED,
    HCD_PORT_STATE_DISABLED,
    HCD_PORT_STATE_RESETTING,
    HCD_PORT_STATE_SUSPENDED,
    HCD_PORT_STATE_RESUMING,
    HCD_PORT_STATE_ENABLED,
    HCD_PORT_STATE_RECOVERY,
} hcd_port_state_t;

typedef enum {
    HCD_PIPE_STATE_ACTIVE,
    HCD_PIPE_STATE_HALTED,
} hcd_pipe_state_t;

typedef enum {
    HCD_PORT_EVENT_NONE,
    HCD_PORT_EVENT_CONNECTION,
    HCD_PORT_EVENT_DISCONNECTION,
    HCD_PORT_EVENT_ERROR,
    HCD_PORT_EVENT_OVERCURRENT,
} hcd_port_event_t;

typedef enum {
    HCD_PIPE_EVENT_NONE,
    HCD_PIPE_EVENT_URB_DONE,
    HCD_PIPE_EVENT_ERROR_XFER,
    HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL,
    HCD_PIPE_EVENT_ERROR_OVERFLOW,
    HCD_PIPE_EVENT_ERROR_STALL,
} hcd_pipe_event_t;

typedef enum {
    HCD_PORT_CMD_POWER_ON,
    HCD_PORT_CMD_POWER_OFF,
    HCD_PORT_CMD_RESET,
    HCD_PORT_CMD_SUSPEND,
    HCD_PORT_CMD_RESUME,
    HCD_PORT_CMD_DISABLE,
} hcd_port_cmd_t;

typedef enum {
    HCD_PIPE_CMD_HALT,
    HCD_PIPE_CMD_FLUSH,
    HCD_PIPE_CMD_CLEAR,
} hcd_pipe_cmd_t;

typedef void *hcd_port_handle_t;
typedef void *hcd_pipe_handle_t;
typedef bool (*hcd_port_callback_t)(hcd_port_handle_t port_hdl, hcd_port_event_t port_event, void *user_arg, bool in_isr);
typedef bool (*hcd_pipe_callback_t)(hcd_pipe_handle_t pipe_hdl, hcd_pipe_event_t pipe_event, void *user_arg, bool in_isr);

typedef enum {
    HCD_PORT_FIFO_BIAS_BALANCED,
    HCD_PORT_FIFO_BIAS_RX,
    HCD_PORT_FIFO_BIAS_PTX,
} hcd_port_fifo_bias_t;

typedef struct {
    int intr_flags;
} hcd_config_t;

typedef struct {
    hcd_port_fifo_bias_t fifo_bias;
    hcd_port_callback_t callback;
    void *callback_arg;
    void *context;
} hcd_port_config_t;

typedef struct {
    hcd_pipe_callback_t callback;
    void *callback_arg;
    void *context;
    const usb_ep_desc_t *ep_desc;   // NULL = default (EP0) pipe
    usb_speed_t dev_speed;
    uint8_t dev_addr;
} hcd_pipe_config_t;

esp_err_t hcd_install(const hcd_config_t *config);
esp_err_t hcd_uninstall(void);
esp_err_t hcd_port_init(int port_number, const hcd_port_config_t *port_config, hcd_port_handle_t *port_hdl);
esp_err_t hcd_port_deinit(hcd_port_handle_t port_hdl);
esp_err_t hcd_port_command(hcd_port_handle_t port_hdl, hcd_port_cmd_t command);
hcd_port_state_t hcd_port_get_state(hcd_port_handle_t port_hdl);
esp_err_t hcd_port_get_speed(hcd_port_handle_t port_hdl, usb_speed_t *speed);
hcd_port_event_t hcd_port_handle_event(hcd_port_handle_t port_hdl);
esp_err_t hcd_port_recover(hcd_port_handle_t port_hdl);
esp_err_t hcd_pipe_alloc(hcd_port_handle_t port_hdl, const hcd_pipe_config_t *pipe_config, hcd_pipe_handle_t *pipe_hdl);
esp_err_t hcd_pipe_free(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_pipe_update_mps(hcd_pipe_handle_t pipe_hdl, int mps);
esp_err_t hcd_pipe_update_dev_addr(hcd_pipe_handle_t pipe_hdl, uint8_t dev_addr);
hcd_pipe_state_t hcd_pipe_get_state(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_pipe_command(hcd_pipe_handle_t pipe_hdl, hcd_pipe_cmd_t command);
hcd_pipe_event_t hcd_pipe_get_event(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_urb_enqueue(hcd_pipe_handle_t pipe_hdl, urb_t *urb);
urb_t *hcd_urb_dequeue(hcd_pipe_handle_t pipe_hdl);
esp_err_t hcd_urb_abort(urb_t *urb);

#ifdef __cplusplus
}
#endif
