/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_strip_encoder.h"

#include <stdlib.h>

#include "esp_check.h"

static const char *TAG = "faces_led_encoder";

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_led_strip_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t encode_led_strip(rmt_encoder_t *encoder,
                               rmt_channel_handle_t channel,
                               const void *primary_data,
                               size_t data_size,
                               rmt_encode_state_t *ret_state)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += led_encoder->bytes_encoder->encode(led_encoder->bytes_encoder,
                                                               channel,
                                                               primary_data,
                                                               data_size,
                                                               &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        __attribute__((fallthrough));
    case 1:
        encoded_symbols += led_encoder->copy_encoder->encode(led_encoder->copy_encoder,
                                                              channel,
                                                              &led_encoder->reset_code,
                                                              sizeof(led_encoder->reset_code),
                                                              &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
        break;
    default:
        led_encoder->state = RMT_ENCODING_RESET;
        break;
    }

out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t delete_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t reset_led_strip_encoder(rmt_encoder_t *encoder)
{
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config,
                                    rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    rmt_led_strip_encoder_t *led_encoder = NULL;
    ESP_GOTO_ON_FALSE(config != NULL && ret_encoder != NULL,
                      ESP_ERR_INVALID_ARG,
                      error,
                      TAG,
                      "invalid argument");

    led_encoder = rmt_alloc_encoder_mem(sizeof(rmt_led_strip_encoder_t));
    ESP_GOTO_ON_FALSE(led_encoder != NULL, ESP_ERR_NO_MEM, error, TAG, "no memory");
    led_encoder->base.encode = encode_led_strip;
    led_encoder->base.del = delete_led_strip_encoder;
    led_encoder->base.reset = reset_led_strip_encoder;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * config->resolution / 1000000,
            .level1 = 0,
            .duration1 = 0.9 * config->resolution / 1000000,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * config->resolution / 1000000,
            .level1 = 0,
            .duration1 = 0.3 * config->resolution / 1000000,
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_config, &led_encoder->bytes_encoder),
                      error,
                      TAG,
                      "create bytes encoder");

    rmt_copy_encoder_config_t copy_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_config, &led_encoder->copy_encoder),
                      error,
                      TAG,
                      "create copy encoder");

    const uint32_t reset_ticks = config->resolution / 1000000 * 50 / 2;
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    *ret_encoder = &led_encoder->base;
    return ESP_OK;

error:
    if (led_encoder != NULL) {
        if (led_encoder->bytes_encoder != NULL) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder != NULL) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}
