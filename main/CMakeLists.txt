idf_component_register(
    SRCS "main.c" "bmb_ui.c" "bmb_state.c" "bmb_report.c" "bmb_json.c"
        "bmb_command.c" "bmb_rx.c" "bmb_transport_policy.c" "bmb_config.c"
        "bmb_buzz.c" "bmb_mqtt.c" "board_display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash mqtt esp_netif esp_wifi
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd esp_codec_dev)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
