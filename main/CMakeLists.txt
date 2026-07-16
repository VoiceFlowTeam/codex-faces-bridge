idf_component_register(
    SRCS
        "app_main.c"
        "board_fire_faces.c"
        "codex_hid.c"
        "codex_protocol.c"
        "led_strip_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES bt driver esp_hid json nvs_flash
)
