#pragma once

#include <stdbool.h>

#include "codex_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_BUTTON_UP = 0,
    BOARD_BUTTON_DOWN,
    BOARD_BUTTON_LEFT,
    BOARD_BUTTON_RIGHT,
    BOARD_BUTTON_GAME_A,
    BOARD_BUTTON_GAME_B,
    BOARD_BUTTON_START,
    BOARD_BUTTON_PAUSE,
    BOARD_BUTTON_FIRE_A,
    BOARD_BUTTON_FIRE_B,
    BOARD_BUTTON_FIRE_C,
    BOARD_BUTTON_COUNT,
} board_button_t;

typedef void (*board_button_fn)(board_button_t button, bool pressed, void *context);

esp_err_t board_fire_faces_init(board_button_fn button_fn, void *button_context);
void board_fire_faces_set_connected(bool connected);
void board_fire_faces_set_agent_layer(bool enabled);
void board_fire_faces_apply_lighting(const codex_lighting_state_t *state, void *context);

#ifdef __cplusplus
}
#endif
