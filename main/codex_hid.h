#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*codex_hid_rx_fn)(const uint8_t *data, size_t length, void *context);
typedef void (*codex_hid_connection_fn)(bool connected, void *context);

esp_err_t codex_hid_init(codex_hid_rx_fn rx_fn,
                         void *rx_context,
                         codex_hid_connection_fn connection_fn,
                         void *connection_context);

esp_err_t codex_hid_send_text(const char *text, size_t length, void *context);
bool codex_hid_is_connected(void);

#ifdef __cplusplus
}
#endif
