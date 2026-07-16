#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEX_THREAD_COUNT 6

typedef struct {
    uint8_t id;
    uint32_t color;
    float brightness;
    uint8_t effect;
    float speed;
} codex_thread_lighting_t;

typedef struct {
    codex_thread_lighting_t threads[CODEX_THREAD_COUNT];
    uint32_t ambient_color;
    float ambient_brightness;
    uint8_t ambient_effect;
} codex_lighting_state_t;

typedef esp_err_t (*codex_protocol_send_fn)(const char *text, size_t length, void *context);
typedef void (*codex_protocol_lighting_fn)(const codex_lighting_state_t *state, void *context);

esp_err_t codex_protocol_init(codex_protocol_send_fn send_fn,
                              void *send_context,
                              codex_protocol_lighting_fn lighting_fn,
                              void *lighting_context);

void codex_protocol_feed(const uint8_t *data, size_t length);

esp_err_t codex_protocol_send_key(const char *key, bool pressed, int agent_index);
esp_err_t codex_protocol_send_joystick(float angle, float distance);

#ifdef __cplusplus
}
#endif
