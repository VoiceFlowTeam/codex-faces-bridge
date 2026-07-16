#include "board_fire_faces.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"

#define FIRE_I2C_SDA GPIO_NUM_21
#define FIRE_I2C_SCL GPIO_NUM_22
#define FIRE_I2C_PORT I2C_NUM_0
#define FACES_GAMEPAD_ADDRESS 0x08
#define FIRE_IP5306_ADDRESS 0x75
#define IP5306_REG_CHARGE_STATUS 0x70
#define IP5306_REG_BATTERY_LEVEL 0x78
#define POWER_POLL_INTERVAL_MS 30000

#define FIRE_BUTTON_A GPIO_NUM_39
#define FIRE_BUTTON_B GPIO_NUM_38
#define FIRE_BUTTON_C GPIO_NUM_37

#define FACES_RGB_GPIO GPIO_NUM_15
#define FACES_RGB_COUNT 10
#define FACES_RGB_PER_SIDE 5
#define FACES_LED_MASTER_BRIGHTNESS 0.20f
#define RGB_RMT_RESOLUTION_HZ 10000000
#define LIGHTING_FRAME_INTERVAL_MS 40
#define SCREEN_ANIMATION_INTERVAL_MS 500

#define FIRE_LCD_HOST SPI3_HOST
#define FIRE_LCD_MOSI GPIO_NUM_23
#define FIRE_LCD_MISO GPIO_NUM_19
#define FIRE_LCD_CLK GPIO_NUM_18
#define FIRE_LCD_CS GPIO_NUM_14
#define FIRE_LCD_DC GPIO_NUM_27
#define FIRE_LCD_RST GPIO_NUM_33
#define FIRE_LCD_BACKLIGHT GPIO_NUM_32
#define FIRE_LCD_WIDTH 320
#define FIRE_LCD_HEIGHT 240

static const char *TAG = "fire_faces";

static board_button_fn s_button_fn;
static void *s_button_context;
static board_power_fn s_power_fn;
static void *s_power_context;
static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static spi_device_handle_t s_lcd;
static SemaphoreHandle_t s_ui_mutex;
static codex_lighting_state_t s_lighting;
static bool s_connected;
static bool s_agent_layer;
static bool s_led_available;
static bool s_lcd_available;
static bool s_gamepad_available;
static bool s_gamepad_error_logged;
static bool s_power_error_logged;
static bool s_lighting_received;
static TickType_t s_next_gamepad_retry;
static codex_power_status_t s_power_status;

static const uint8_t s_digit_3x5[6][5] = {
    {0b010, 0b110, 0b010, 0b010, 0b111},
    {0b110, 0b001, 0b010, 0b100, 0b111},
    {0b110, 0b001, 0b010, 0b001, 0b110},
    {0b101, 0b101, 0b111, 0b001, 0b001},
    {0b111, 0b100, 0b110, 0b001, 0b110},
    {0b011, 0b100, 0b111, 0b101, 0b111},
};

static uint16_t rgb888_to_565(uint32_t color)
{
    const uint8_t red = (color >> 16) & 0xFF;
    const uint8_t green = (color >> 8) & 0xFF;
    const uint8_t blue = color & 0xFF;
    return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

static uint32_t apply_brightness(uint32_t color, float brightness)
{
    if (brightness < 0.0f) {
        brightness = 0.0f;
    } else if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    const uint8_t red = (uint8_t)(((color >> 16) & 0xFF) * brightness);
    const uint8_t green = (uint8_t)(((color >> 8) & 0xFF) * brightness);
    const uint8_t blue = (uint8_t)((color & 0xFF) * brightness);
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

static float clamp_unit(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

static uint32_t blend_rgb(uint32_t from, uint32_t to, float amount)
{
    amount = clamp_unit(amount);
    const float inverse = 1.0f - amount;
    const uint8_t red = (uint8_t)((((from >> 16) & 0xFF) * inverse) + (((to >> 16) & 0xFF) * amount));
    const uint8_t green = (uint8_t)((((from >> 8) & 0xFF) * inverse) + (((to >> 8) & 0xFF) * amount));
    const uint8_t blue = (uint8_t)(((from & 0xFF) * inverse) + ((to & 0xFF) * amount));
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

static uint32_t hsv_to_rgb(float hue, float saturation, float value)
{
    hue = hue - floorf(hue);
    saturation = clamp_unit(saturation);
    value = clamp_unit(value);
    const float scaled = hue * 6.0f;
    const int sector = (int)floorf(scaled);
    const float fraction = scaled - sector;
    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - saturation * fraction);
    const float t = value * (1.0f - saturation * (1.0f - fraction));
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    switch (sector % 6) {
    case 0:
        red = value; green = t; blue = p;
        break;
    case 1:
        red = q; green = value; blue = p;
        break;
    case 2:
        red = p; green = value; blue = t;
        break;
    case 3:
        red = p; green = q; blue = value;
        break;
    case 4:
        red = t; green = p; blue = value;
        break;
    default:
        red = value; green = p; blue = q;
        break;
    }
    return ((uint32_t)(red * 255.0f) << 16) |
           ((uint32_t)(green * 255.0f) << 8) |
           (uint32_t)(blue * 255.0f);
}

static float lighting_phase(float speed, int64_t now_ms)
{
    const float cycles_per_second = 0.25f + clamp_unit(speed) * 1.75f;
    return fmodf((now_ms / 1000.0f) * cycles_per_second, 1.0f);
}

static uint32_t render_effect(uint32_t color,
                              float brightness,
                              uint8_t effect,
                              float speed,
                              float position,
                              int64_t now_ms)
{
    brightness = clamp_unit(brightness);
    position = clamp_unit(position);
    if (effect == CODEX_LIGHTING_OFF || brightness <= 0.0f) {
        return 0;
    }
    if (effect == CODEX_LIGHTING_SOLID) {
        return apply_brightness(color, brightness);
    }

    const float phase = lighting_phase(speed, now_ms);
    switch (effect) {
    case CODEX_LIGHTING_SNAKE: {
        float distance = fabsf(position - phase);
        distance = fminf(distance, 1.0f - distance);
        const float pulse = distance < 0.30f ? 1.0f - (distance / 0.30f) : 0.04f;
        return apply_brightness(color, brightness * pulse);
    }
    case CODEX_LIGHTING_RAINBOW:
        return hsv_to_rgb(phase + position, 1.0f, brightness);
    case CODEX_LIGHTING_BREATH: {
        const float pulse = 0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI);
        return apply_brightness(color, brightness * pulse);
    }
    case CODEX_LIGHTING_GRADIENT:
        return apply_brightness(blend_rgb(color, 0xFFFFFF, position * 0.45f), brightness);
    case CODEX_LIGHTING_SHALLOW_BREATH: {
        const float pulse = 0.75f - 0.25f * cosf(phase * 2.0f * (float)M_PI);
        return apply_brightness(color, brightness * pulse);
    }
    default:
        return apply_brightness(color, brightness);
    }
}

static bool effect_is_animated(uint8_t effect)
{
    return effect == CODEX_LIGHTING_SNAKE ||
           effect == CODEX_LIGHTING_RAINBOW ||
           effect == CODEX_LIGHTING_BREATH ||
           effect == CODEX_LIGHTING_SHALLOW_BREATH;
}

static uint32_t render_thread_color(const codex_thread_lighting_t *thread, int index, int64_t now_ms)
{
    return render_effect(thread->color,
                         thread->brightness,
                         thread->effect,
                         thread->speed,
                         (float)index / CODEX_THREAD_COUNT,
                         now_ms);
}

static uint32_t render_side_color(const codex_lighting_side_t *side,
                                  int index,
                                  int count,
                                  int64_t now_ms)
{
    const float position = count <= 1 ? 0.0f : (float)index / count;
    return render_effect(side->color,
                         side->brightness,
                         side->effect,
                         side->speed,
                         position,
                         now_ms);
}

static uint32_t render_thread_palette_color(int palette_index, int64_t now_ms)
{
    const float source = (float)palette_index * (CODEX_THREAD_COUNT - 1) /
                         (FACES_RGB_PER_SIDE - 1);
    const int lower = (int)floorf(source);
    const int upper = lower + 1 < CODEX_THREAD_COUNT ? lower + 1 : lower;
    const float amount = source - lower;
    const uint32_t lower_color = render_thread_color(&s_lighting.threads[lower], lower, now_ms);
    const uint32_t upper_color = render_thread_color(&s_lighting.threads[upper], upper, now_ms);
    return blend_rgb(lower_color, upper_color, amount);
}

static esp_err_t lcd_send(bool command, const void *data, size_t length)
{
    if (s_lcd == NULL || data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_set_level(FIRE_LCD_DC, command ? 0 : 1);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_lcd, &transaction);
}

static esp_err_t lcd_command(uint8_t command, const uint8_t *params, size_t param_length)
{
    ESP_RETURN_ON_ERROR(lcd_send(true, &command, 1), TAG, "LCD command 0x%02x", command);
    if (params != NULL && param_length > 0) {
        ESP_RETURN_ON_ERROR(lcd_send(false, params, param_length), TAG, "LCD params 0x%02x", command);
    }
    return ESP_OK;
}

static esp_err_t lcd_set_window(int x, int y, int width, int height)
{
    const uint16_t x_end = x + width - 1;
    const uint16_t y_end = y + height - 1;
    const uint8_t columns[] = {
        (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x_end >> 8), (uint8_t)x_end,
    };
    const uint8_t rows[] = {
        (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y_end >> 8), (uint8_t)y_end,
    };
    ESP_RETURN_ON_ERROR(lcd_command(0x2A, columns, sizeof(columns)), TAG, "set LCD columns");
    ESP_RETURN_ON_ERROR(lcd_command(0x2B, rows, sizeof(rows)), TAG, "set LCD rows");
    return lcd_command(0x2C, NULL, 0);
}

static void lcd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!s_lcd_available || width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > FIRE_LCD_WIDTH) {
        width = FIRE_LCD_WIDTH - x;
    }
    if (y + height > FIRE_LCD_HEIGHT) {
        height = FIRE_LCD_HEIGHT - y;
    }
    if (width <= 0 || height <= 0 || lcd_set_window(x, y, width, height) != ESP_OK) {
        return;
    }

    uint8_t line[FIRE_LCD_WIDTH * 2];
    for (int column = 0; column < width; ++column) {
        line[column * 2] = color >> 8;
        line[column * 2 + 1] = color & 0xFF;
    }
    for (int row = 0; row < height; ++row) {
        if (lcd_send(false, line, width * 2) != ESP_OK) {
            break;
        }
    }
}

static void lcd_draw_digit(int digit, int center_x, int center_y, uint16_t color)
{
    if (digit < 0 || digit >= 6) {
        return;
    }
    const int scale = 6;
    const int origin_x = center_x - (3 * scale) / 2;
    const int origin_y = center_y - (5 * scale) / 2;
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (s_digit_3x5[digit][row] & (1 << (2 - column))) {
                lcd_fill_rect(origin_x + column * scale,
                              origin_y + row * scale,
                              scale - 1,
                              scale - 1,
                              color);
            }
        }
    }
}

static void lcd_draw_battery(uint16_t background)
{
    const int x = 286;
    const int y = 7;
    const int body_width = 25;
    const int body_height = 14;
    const uint16_t outline = 0xFFFF;

    lcd_fill_rect(x, y, body_width, body_height, outline);
    lcd_fill_rect(x + 2, y + 2, body_width - 4, body_height - 4, background);
    lcd_fill_rect(x + body_width, y + 4, 3, body_height - 8, outline);

    if (!s_power_status.available) {
        lcd_fill_rect(x + 7, y + 6, 11, 2, rgb888_to_565(0xFF5C5C));
        return;
    }

    int segments = (s_power_status.battery_percent + 24) / 25;
    if (segments < 0) {
        segments = 0;
    } else if (segments > 4) {
        segments = 4;
    }
    uint16_t fill = outline;
    if (s_power_status.is_charging) {
        fill = rgb888_to_565(0x42E879);
    } else if (s_power_status.battery_percent <= 25) {
        fill = rgb888_to_565(0xFF5C5C);
    }
    for (int segment = 0; segment < segments; ++segment) {
        lcd_fill_rect(x + 3 + segment * 5, y + 3, 4, body_height - 6, fill);
    }
}

static esp_err_t init_lcd(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << FIRE_LCD_DC) | (1ULL << FIRE_LCD_RST) | (1ULL << FIRE_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "configure LCD GPIO");
    gpio_set_level(FIRE_LCD_BACKLIGHT, 0);
    gpio_set_level(FIRE_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(FIRE_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus_config = {
        .mosi_io_num = FIRE_LCD_MOSI,
        .miso_io_num = FIRE_LCD_MISO,
        .sclk_io_num = FIRE_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FIRE_LCD_WIDTH * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(FIRE_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "initialize LCD SPI bus");

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 26 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = FIRE_LCD_CS,
        .queue_size = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(FIRE_LCD_HOST, &device_config, &s_lcd), TAG, "attach LCD");

    // M5Stack FIRE uses an ILI9342C panel with a native 320x240 address space.
    // Keep row/column exchange disabled so the logical framebuffer dimensions
    // match the controller window exactly.
    const uint8_t extension[] = {0xFF, 0x93, 0x42};
    const uint8_t pwr1[] = {0x12, 0x12};
    const uint8_t pwr2[] = {0x03};
    const uint8_t vcom[] = {0xF2};
    const uint8_t interface_mode[] = {0xE0};
    const uint8_t interface_control[] = {0x01, 0x00, 0x00};
    const uint8_t madctl[] = {0x08}; // BGR, native landscape 320x240
    const uint8_t pixel_format[] = {0x55};
    const uint8_t display_control[] = {0x08, 0x82, 0x1D, 0x04};
    const uint8_t gamma_pos[] = {0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F};
    const uint8_t gamma_neg[] = {0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F};

    ESP_RETURN_ON_ERROR(lcd_command(0x01, NULL, 0), TAG, "reset LCD");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lcd_command(0x28, NULL, 0), TAG, "disable LCD");
    ESP_RETURN_ON_ERROR(lcd_command(0xC8, extension, sizeof(extension)), TAG, "LCD extended commands");
    ESP_RETURN_ON_ERROR(lcd_command(0xC0, pwr1, sizeof(pwr1)), TAG, "LCD power 1");
    ESP_RETURN_ON_ERROR(lcd_command(0xC1, pwr2, sizeof(pwr2)), TAG, "LCD power 2");
    ESP_RETURN_ON_ERROR(lcd_command(0xC5, vcom, sizeof(vcom)), TAG, "LCD VCOM");
    ESP_RETURN_ON_ERROR(lcd_command(0xB0, interface_mode, sizeof(interface_mode)), TAG, "LCD interface mode");
    ESP_RETURN_ON_ERROR(lcd_command(0xF6, interface_control, sizeof(interface_control)), TAG, "LCD interface control");
    ESP_RETURN_ON_ERROR(lcd_command(0x36, madctl, sizeof(madctl)), TAG, "LCD orientation");
    ESP_RETURN_ON_ERROR(lcd_command(0x3A, pixel_format, sizeof(pixel_format)), TAG, "LCD pixel format");
    ESP_RETURN_ON_ERROR(lcd_command(0xB6, display_control, sizeof(display_control)), TAG, "LCD display control");
    ESP_RETURN_ON_ERROR(lcd_command(0xE0, gamma_pos, sizeof(gamma_pos)), TAG, "LCD positive gamma");
    ESP_RETURN_ON_ERROR(lcd_command(0xE1, gamma_neg, sizeof(gamma_neg)), TAG, "LCD negative gamma");
    ESP_RETURN_ON_ERROR(lcd_command(0x38, NULL, 0), TAG, "LCD idle off");
    ESP_RETURN_ON_ERROR(lcd_command(0x21, NULL, 0), TAG, "LCD inversion on");
    ESP_RETURN_ON_ERROR(lcd_command(0x11, NULL, 0), TAG, "LCD sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(lcd_command(0x29, NULL, 0), TAG, "LCD display on");
    gpio_set_level(FIRE_LCD_BACKLIGHT, 1);
    s_lcd_available = true;
    lcd_fill_rect(0, 0, FIRE_LCD_WIDTH, FIRE_LCD_HEIGHT, 0x0000);
    return ESP_OK;
}

static esp_err_t init_leds(void)
{
    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = FACES_RGB_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RGB_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_channel), TAG, "create RGB RMT channel");

    led_strip_encoder_config_t encoder_config = {
        .resolution = RGB_RMT_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder), TAG, "create RGB encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "enable RGB RMT channel");
    s_led_available = true;
    return ESP_OK;
}

static void flush_leds_locked(int64_t now_ms)
{
    if (!s_led_available) {
        return;
    }
    uint32_t palette[FACES_RGB_PER_SIDE] = {0};
    for (int position = 0; position < FACES_RGB_PER_SIDE; ++position) {
        const uint32_t thread = render_thread_palette_color(position, now_ms);
        const uint32_t ambient = render_side_color(&s_lighting.ambient,
                                                    position,
                                                    FACES_RGB_PER_SIDE,
                                                    now_ms);
        uint32_t color = thread != 0 && ambient != 0
                             ? blend_rgb(ambient, thread, 0.82f)
                             : (thread != 0 ? thread : ambient);
        if (color == 0 && s_connected && !s_lighting_received) {
            color = 0x2457FF;
        }
        palette[position] = apply_brightness(color, FACES_LED_MASTER_BRIGHTNESS);
    }

    uint8_t pixels[FACES_RGB_COUNT * 3] = {0};
    for (int led = 0; led < FACES_RGB_COUNT; ++led) {
        // Mirror the second five-LED group for a balanced two-sided palette.
        const int position = led < FACES_RGB_PER_SIDE ? led : FACES_RGB_COUNT - 1 - led;
        const uint32_t color = palette[position];
        pixels[led * 3] = (color >> 8) & 0xFF;      // G
        pixels[led * 3 + 1] = (color >> 16) & 0xFF; // R
        pixels[led * 3 + 2] = color & 0xFF;         // B
    }

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };
    if (rmt_transmit(s_led_channel, s_led_encoder, pixels, sizeof(pixels), &transmit_config) == ESP_OK) {
        rmt_tx_wait_all_done(s_led_channel, 200);
    }
}

static void refresh_screen_locked(int64_t now_ms)
{
    if (!s_lcd_available) {
        return;
    }
    uint32_t header_rgb = s_agent_layer ? 0xFF8A33 : 0x3E63DD;
    const uint32_t keys_rgb = render_side_color(&s_lighting.keys, 0, 1, now_ms);
    if (keys_rgb != 0) {
        header_rgb = keys_rgb;
    }
    const uint16_t header = rgb888_to_565(header_rgb);
    const uint16_t header_background = s_connected ? header : rgb888_to_565(0x30333A);
    lcd_fill_rect(0, 0, FIRE_LCD_WIDTH, 28, header_background);
    lcd_draw_battery(header_background);
    lcd_fill_rect(0, 28, FIRE_LCD_WIDTH, FIRE_LCD_HEIGHT - 28, rgb888_to_565(0x101216));

    const int tile_width = 96;
    const int tile_height = 88;
    for (int i = 0; i < CODEX_THREAD_COUNT; ++i) {
        const int column = i % 3;
        const int row = i / 3;
        const int x = 8 + column * 104;
        const int y = 38 + row * 98;
        const codex_thread_lighting_t *thread = &s_lighting.threads[i];
        uint32_t tile_rgb = render_thread_color(thread, i, now_ms);
        if (tile_rgb == 0) {
            tile_rgb = 0x24272E;
        }
        const uint16_t tile = rgb888_to_565(tile_rgb);
        const uint16_t border = (thread->effect == CODEX_LIGHTING_BREATH ||
                                 thread->effect == CODEX_LIGHTING_SHALLOW_BREATH)
                                    ? 0xFFFF
                                    : rgb888_to_565(0x5B606B);
        lcd_fill_rect(x, y, tile_width, tile_height, border);
        lcd_fill_rect(x + 3, y + 3, tile_width - 6, tile_height - 6, tile);
        lcd_draw_digit(i, x + tile_width / 2, y + tile_height / 2, 0xFFFF);
    }
}

static bool lighting_is_animated_locked(void)
{
    if (effect_is_animated(s_lighting.keys.effect) ||
        effect_is_animated(s_lighting.ambient.effect)) {
        return true;
    }
    for (int i = 0; i < CODEX_THREAD_COUNT; ++i) {
        if (effect_is_animated(s_lighting.threads[i].effect)) {
            return true;
        }
    }
    return false;
}

static bool screen_is_animated_locked(void)
{
    if (effect_is_animated(s_lighting.keys.effect)) {
        return true;
    }
    for (int i = 0; i < CODEX_THREAD_COUNT; ++i) {
        if (effect_is_animated(s_lighting.threads[i].effect)) {
            return true;
        }
    }
    return false;
}

static esp_err_t init_i2c(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = FIRE_I2C_SDA,
        .scl_io_num = FIRE_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(FIRE_I2C_PORT, &config), TAG, "configure I2C bus");
    return i2c_driver_install(FIRE_I2C_PORT, config.mode, 0, 0, 0);
}

static esp_err_t ip5306_read_register(uint8_t reg, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_write_read_device(FIRE_I2C_PORT,
                                        FIRE_IP5306_ADDRESS,
                                        &reg,
                                        sizeof(reg),
                                        value,
                                        1,
                                        pdMS_TO_TICKS(50));
}

static int decode_ip5306_battery_level(uint8_t raw)
{
    switch (raw >> 4) {
    case 0x00:
        return 100;
    case 0x08:
        return 75;
    case 0x0C:
        return 50;
    case 0x0E:
        return 25;
    default:
        return 0;
    }
}

static void update_power_status(void)
{
    uint8_t battery_raw = 0;
    uint8_t charge_raw = 0;
    const esp_err_t battery_result = ip5306_read_register(IP5306_REG_BATTERY_LEVEL, &battery_raw);
    const esp_err_t charge_result = ip5306_read_register(IP5306_REG_CHARGE_STATUS, &charge_raw);
    if (battery_result != ESP_OK || charge_result != ESP_OK) {
        if (!s_power_error_logged) {
            ESP_LOGW(TAG, "IP5306 unavailable (battery=%s, charge=%s)",
                     esp_err_to_name(battery_result),
                     esp_err_to_name(charge_result));
            s_power_error_logged = true;
        }
        return;
    }

    const codex_power_status_t next = {
        .battery_percent = decode_ip5306_battery_level(battery_raw),
        .is_charging = (charge_raw & 0x08) != 0,
        .available = true,
    };
    bool changed = false;
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        changed = !s_power_status.available ||
                  s_power_status.battery_percent != next.battery_percent ||
                  s_power_status.is_charging != next.is_charging;
        s_power_status = next;
        if (changed) {
            refresh_screen_locked(esp_timer_get_time() / 1000);
        }
        xSemaphoreGive(s_ui_mutex);
    }
    s_power_error_logged = false;

    if (changed) {
        ESP_LOGI(TAG, "IP5306 battery %d%%, charging=%s",
                 next.battery_percent,
                 next.is_charging ? "yes" : "no");
        if (s_power_fn != NULL) {
            s_power_fn(&next, s_power_context);
        }
    }
}

static void power_task(void *argument)
{
    (void)argument;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POWER_POLL_INTERVAL_MS));
        update_power_status();
    }
}

static void lighting_task(void *argument)
{
    (void)argument;
    int64_t last_screen_frame_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LIGHTING_FRAME_INTERVAL_MS));
        if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (!lighting_is_animated_locked()) {
            xSemaphoreGive(s_ui_mutex);
            continue;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        flush_leds_locked(now_ms);
        if (screen_is_animated_locked() &&
            now_ms - last_screen_frame_ms >= SCREEN_ANIMATION_INTERVAL_MS) {
            refresh_screen_locked(now_ms);
            last_screen_frame_ms = now_ms;
        }
        xSemaphoreGive(s_ui_mutex);
    }
}

static uint16_t sample_pressed_buttons(void)
{
    uint8_t gamepad_raw = 0xFF;
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_next_gamepad_retry) >= 0) {
        const esp_err_t status = i2c_master_read_from_device(FIRE_I2C_PORT,
                                                             FACES_GAMEPAD_ADDRESS,
                                                             &gamepad_raw,
                                                             sizeof(gamepad_raw),
                                                             pdMS_TO_TICKS(50));
        if (status == ESP_OK) {
            if (!s_gamepad_available) {
                ESP_LOGI(TAG, "FACES GameBoy panel ready at I2C address 0x%02x", FACES_GAMEPAD_ADDRESS);
            }
            s_gamepad_available = true;
            s_gamepad_error_logged = false;
        } else {
            if (!s_gamepad_error_logged) {
                ESP_LOGW(TAG, "FACES GameBoy panel unavailable (%s); retrying once per second",
                         esp_err_to_name(status));
                s_gamepad_error_logged = true;
            }
            s_gamepad_available = false;
            s_next_gamepad_retry = now + pdMS_TO_TICKS(1000);
        }
    }

    uint16_t pressed = (uint16_t)(~gamepad_raw) & 0xFF;
    if (gpio_get_level(FIRE_BUTTON_A) == 0) {
        pressed |= 1U << BOARD_BUTTON_FIRE_A;
    }
    if (gpio_get_level(FIRE_BUTTON_B) == 0) {
        pressed |= 1U << BOARD_BUTTON_FIRE_B;
    }
    if (gpio_get_level(FIRE_BUTTON_C) == 0) {
        pressed |= 1U << BOARD_BUTTON_FIRE_C;
    }
    return pressed;
}

static void button_task(void *argument)
{
    (void)argument;
    uint16_t stable = sample_pressed_buttons();
    uint16_t candidate = stable;
    unsigned candidate_count = 0;

    while (true) {
        const uint16_t sample = sample_pressed_buttons();
        if (sample != candidate) {
            candidate = sample;
            candidate_count = 0;
        } else if (candidate_count < 3) {
            ++candidate_count;
        }

        if (candidate_count >= 2 && candidate != stable) {
            const uint16_t changed = candidate ^ stable;
            stable = candidate;
            for (int button = 0; button < BOARD_BUTTON_COUNT; ++button) {
                if ((changed & (1U << button)) != 0 && s_button_fn != NULL) {
                    s_button_fn((board_button_t)button,
                                (stable & (1U << button)) != 0,
                                s_button_context);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t board_fire_faces_init(board_button_fn button_fn,
                                void *button_context,
                                board_power_fn power_fn,
                                void *power_context)
{
    if (button_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_button_fn = button_fn;
    s_button_context = button_context;
    s_power_fn = power_fn;
    s_power_context = power_context;
    s_ui_mutex = xSemaphoreCreateMutex();
    if (s_ui_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << FIRE_BUTTON_A) | (1ULL << FIRE_BUTTON_B) | (1ULL << FIRE_BUTTON_C),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "configure FIRE buttons");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "initialize FACES GameBoy panel");
    update_power_status();

    esp_err_t status = init_leds();
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "FACES II RGB unavailable: %s", esp_err_to_name(status));
    } else {
        ESP_LOGI(TAG, "FACES II RGB strip ready on GPIO %d", FACES_RGB_GPIO);
    }
    status = init_lcd();
    if (status != ESP_OK) {
        ESP_LOGW(TAG, "FIRE LCD unavailable: %s", esp_err_to_name(status));
    } else {
        ESP_LOGI(TAG, "FIRE LCD ready");
    }

    if (xTaskCreate(button_task, "faces_buttons", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(power_task, "fire_power", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(lighting_task, "faces_lighting", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        flush_leds_locked(now_ms);
        refresh_screen_locked(now_ms);
        xSemaphoreGive(s_ui_mutex);
    }
    return ESP_OK;
}

void board_fire_faces_set_connected(bool connected)
{
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    s_connected = connected;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    flush_leds_locked(now_ms);
    refresh_screen_locked(now_ms);
    xSemaphoreGive(s_ui_mutex);
}

void board_fire_faces_set_agent_layer(bool enabled)
{
    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    s_agent_layer = enabled;
    refresh_screen_locked(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_ui_mutex);
}

void board_fire_faces_apply_lighting(const codex_lighting_state_t *state, void *context)
{
    (void)context;
    if (state == NULL || xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    s_lighting = *state;
    s_lighting_received = true;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    flush_leds_locked(now_ms);
    refresh_screen_locked(now_ms);
    xSemaphoreGive(s_ui_mutex);
}

void board_fire_faces_get_power_status(codex_power_status_t *status, void *context)
{
    (void)context;
    if (status == NULL) {
        return;
    }
    *status = (codex_power_status_t){0};
    if (s_ui_mutex != NULL && xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        *status = s_power_status;
        xSemaphoreGive(s_ui_mutex);
    }
}
