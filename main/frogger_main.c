/**
 * @file frogger_main.c
 * @brief Application entry point — namebadge_frogger
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "frogger_game.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "namebadge_frogger starting");

    // NVS init (reserved for high-score persistence)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Init display + game
    ESP_ERROR_CHECK(frogger_init());

    ESP_LOGI(TAG, "Entering game loop (~60 Hz)");

    while (1) {
        frogger_game_loop();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
