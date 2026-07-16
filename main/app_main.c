#include <stdbool.h>
#include <stdint.h>

#include "board_fire_faces.h"
#include "codex_hid.h"
#include "codex_protocol.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define AGENT_LAYER_HOLD_US (700 * 1000)

static const char *TAG = "codex_faces";

static bool s_agent_layer;
static bool s_direction_pressed[4];
static bool s_direction_suppressed[4];
static const char *s_game_active_key[2];
static int s_game_active_agent[2] = {-1, -1};
static bool s_game_suppressed[2];
static int64_t s_pause_pressed_at;

static void send_key(const char *key, bool pressed, int agent_index)
{
    const esp_err_t status = codex_protocol_send_key(key, pressed, agent_index);
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to send %s %s: %s",
                 key,
                 pressed ? "press" : "release",
                 esp_err_to_name(status));
    }
}

static void send_direction_state(void)
{
    const int x = (s_direction_pressed[BOARD_BUTTON_RIGHT] ? 1 : 0) -
                  (s_direction_pressed[BOARD_BUTTON_LEFT] ? 1 : 0);
    const int y = (s_direction_pressed[BOARD_BUTTON_DOWN] ? 1 : 0) -
                  (s_direction_pressed[BOARD_BUTTON_UP] ? 1 : 0);

    float angle = 0.0f;
    if (x == 1 && y == -1) {
        angle = 0.125f;
    } else if (x == 1 && y == 0) {
        angle = 0.25f;
    } else if (x == 1 && y == 1) {
        angle = 0.375f;
    } else if (x == 0 && y == 1) {
        angle = 0.5f;
    } else if (x == -1 && y == 1) {
        angle = 0.625f;
    } else if (x == -1 && y == 0) {
        angle = 0.75f;
    } else if (x == -1 && y == -1) {
        angle = 0.875f;
    }

    const esp_err_t status = codex_protocol_send_joystick(angle, (x == 0 && y == 0) ? 0.0f : 1.0f);
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to send radial input: %s", esp_err_to_name(status));
    }
}

static void handle_direction(board_button_t button, bool pressed)
{
    static const char *agent_keys[] = {"AG00", "AG02", "AG03", "AG01"};
    static const int agent_indices[] = {0, 2, 3, 1};
    const int offset = button - BOARD_BUTTON_UP;

    if (!pressed && s_direction_suppressed[offset]) {
        s_direction_suppressed[offset] = false;
        return;
    }
    if (pressed) {
        s_direction_suppressed[offset] = false;
    }
    s_direction_pressed[offset] = pressed;

    if (s_agent_layer) {
        send_key(agent_keys[offset], pressed, agent_indices[offset]);
        return;
    }

    send_direction_state();
}

static void set_agent_layer(bool enabled)
{
    if (s_agent_layer == enabled) {
        return;
    }

    if (s_agent_layer) {
        static const char *agent_keys[] = {"AG00", "AG02", "AG03", "AG01"};
        static const int agent_indices[] = {0, 2, 3, 1};
        for (int offset = 0; offset < 4; ++offset) {
            if (s_direction_pressed[offset]) {
                send_key(agent_keys[offset], false, agent_indices[offset]);
            }
        }
    } else {
        codex_protocol_send_joystick(0.0f, 0.0f);
    }

    for (int i = 0; i < 4; ++i) {
        s_direction_suppressed[i] = s_direction_pressed[i];
        s_direction_pressed[i] = false;
    }
    for (int i = 0; i < 2; ++i) {
        if (s_game_active_key[i] != NULL) {
            send_key(s_game_active_key[i], false, s_game_active_agent[i]);
            s_game_active_key[i] = NULL;
            s_game_active_agent[i] = -1;
            s_game_suppressed[i] = true;
        }
    }
    s_agent_layer = enabled;
    board_fire_faces_set_agent_layer(enabled);
    ESP_LOGI(TAG, "Agent layer %s", enabled ? "enabled" : "disabled");
}

static void button_event(board_button_t button, bool pressed, void *context)
{
    (void)context;

    if (button >= BOARD_BUTTON_UP && button <= BOARD_BUTTON_RIGHT) {
        handle_direction(button, pressed);
        return;
    }

    if (button == BOARD_BUTTON_PAUSE) {
        if (pressed) {
            s_pause_pressed_at = esp_timer_get_time();
        } else if (s_pause_pressed_at != 0) {
            const int64_t held_us = esp_timer_get_time() - s_pause_pressed_at;
            s_pause_pressed_at = 0;
            if (held_us >= AGENT_LAYER_HOLD_US) {
                set_agent_layer(!s_agent_layer);
            } else {
                send_key("ACT09", true, -1);
                send_key("ACT09", false, -1);
            }
        }
        return;
    }

    if (button == BOARD_BUTTON_GAME_A || button == BOARD_BUTTON_GAME_B) {
        const int index = button - BOARD_BUTTON_GAME_A;
        if (!pressed && s_game_suppressed[index]) {
            s_game_suppressed[index] = false;
            return;
        }
        if (pressed) {
            s_game_suppressed[index] = false;
            s_game_active_key[index] = s_agent_layer ? (index == 0 ? "AG04" : "AG05")
                                                     : (index == 0 ? "ACT06" : "ACT07");
            s_game_active_agent[index] = s_agent_layer ? index + 4 : -1;
            send_key(s_game_active_key[index], true, s_game_active_agent[index]);
        } else if (s_game_active_key[index] != NULL) {
            send_key(s_game_active_key[index], false, s_game_active_agent[index]);
            s_game_active_key[index] = NULL;
            s_game_active_agent[index] = -1;
        }
        return;
    }

    switch (button) {
    case BOARD_BUTTON_START:
        send_key("ACT08", pressed, -1);
        break;
    case BOARD_BUTTON_FIRE_A:
        send_key("ACT10", pressed, -1);
        break;
    case BOARD_BUTTON_FIRE_B:
        send_key("ACT11", pressed, -1);
        break;
    case BOARD_BUTTON_FIRE_C:
        send_key("ACT12", pressed, -1);
        break;
    default:
        break;
    }
}

static void hid_receive(const uint8_t *data, size_t length, void *context)
{
    (void)context;
    codex_protocol_feed(data, length);
}

static void connection_changed(bool connected, void *context)
{
    (void)context;
    board_fire_faces_set_connected(connected);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Codex Micro bridge for M5Stack FIRE + FACES II GameBoy");

    ESP_ERROR_CHECK(codex_protocol_init(codex_hid_send_text,
                                        NULL,
                                        board_fire_faces_apply_lighting,
                                        NULL));
    ESP_ERROR_CHECK(board_fire_faces_init(button_event, NULL));
    ESP_ERROR_CHECK(codex_hid_init(hid_receive, NULL, connection_changed, NULL));
}
