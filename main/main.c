// SPDX-License-Identifier: Apache-2.0

#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "bmb_link.h"
#include "bmb_state.h"
#include "bmb_ui.h"
#include "iris_screen_mirror.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_bundle.h"

static const char *TAG = "bambu_monitor";
static bmb_state_t s_model;
static bmb_ui_t s_ui;

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Keep Recovery reachable even when the external UI image is absent or
     * invalid. */
    iris_ota_support_start();

    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "GSP UI unavailable (0x%x); ESP-Iris Recovery RPC remains active",
                 bundle_err);
        return;
    }

    esp_display_present_target_config_t display;
    ESP_ERROR_CHECK(board_display_init(&display));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_touch_init(&touch));

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;

    ESP_ERROR_CHECK(iris_screen_mirror_init());

    esp_gsp_handle_t ui;
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(ui));
    bmb_state_init(&s_model);
    enum { UI_START_ATTEMPTS = 3, UI_RETRY_DELAY_MS = 500 };
    esp_gsp_err_t ui_err = ESP_GSP_ERR_INVALID_STATE;
    for (unsigned attempt = 0; attempt < UI_START_ATTEMPTS; ++attempt) {
        ui_err = bmb_ui_init(ui, &s_ui, &s_model);
        if (ui_err == ESP_GSP_OK) break;
        ESP_LOGE(TAG, "UI callback/timer setup failed 0x%x (attempt %u/%u)",
                 (unsigned)ui_err, attempt + 1, UI_START_ATTEMPTS);
        if (attempt + 1 < UI_START_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(UI_RETRY_DELAY_MS));
        }
    }
    if (ui_err != ESP_GSP_OK) {
        ESP_LOGE(TAG, "UI inactive; ESP-Iris remains available for diagnosis/update");
        return;
    }
    /* Wi-Fi plus the printer MQTT session; the UI shows the link meanwhile. */
    bmb_link_start(&s_model);

    ESP_LOGI(TAG, "Bambu monitor ready at %dx%d RGB565",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}
