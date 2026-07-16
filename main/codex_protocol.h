#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CODEX_THREAD_COUNT 6

typedef enum {
    CODEX_LIGHTING_OFF = 0,
    CODEX_LIGHTING_SOLID = 1,
    CODEX_LIGHTING_SNAKE = 2,
    CODEX_LIGHTING_RAINBOW = 3,
    CODEX_LIGHTING_BREATH = 4,
    CODEX_LIGHTING_GRADIENT = 5,
    CODEX_LIGHTING_SHALLOW_BREATH = 6,
} codex_lighting_effect_t;

typedef struct {
    uint32_t color;
    float brightness;
    float speed;
    float magic;
    uint8_t effect;
} codex_lighting_side_t;

typedef struct {
    uint8_t id;
    uint32_t color;
    float brightness;
    uint8_t effect;
    float speed;
    bool sync_keys_lighting;
    bool sync_ambient_lighting;
} codex_thread_lighting_t;

typedef struct {
    codex_thread_lighting_t threads[CODEX_THREAD_COUNT];
    codex_lighting_side_t keys;
    codex_lighting_side_t ambient;
} codex_lighting_state_t;

typedef struct {
    int battery_percent;
    bool is_charging;
    bool available;
} codex_power_status_t;

typedef esp_err_t (*codex_protocol_send_fn)(const char *text, size_t length, void *context);
typedef void (*codex_protocol_lighting_fn)(const codex_lighting_state_t *state, void *context);
typedef void (*codex_protocol_power_fn)(codex_power_status_t *status, void *context);

esp_err_t codex_protocol_init(codex_protocol_send_fn send_fn,
                              void *send_context,
                              codex_protocol_lighting_fn lighting_fn,
                              void *lighting_context,
                              codex_protocol_power_fn power_fn,
                              void *power_context);

void codex_protocol_feed(const uint8_t *data, size_t length);

void codex_protocol_set_device_state(int profile_index, int layer_index);
esp_err_t codex_protocol_send_hid_action(const char *key, int action, int agent_index);
esp_err_t codex_protocol_send_key(const char *key, bool pressed, int agent_index);
esp_err_t codex_protocol_send_joystick(float angle, float distance);

#ifdef __cplusplus
}
#endif
