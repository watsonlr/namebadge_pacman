/**
 * @file pacman_main.c
 * @brief Application entry point — namebadge_pacman
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "pacman_game.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "namebadge_pacman starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(pacman_init());

    // Mark this OTA slot valid so the bootloader doesn't roll back on next boot.
    // Safe to call even when running from the factory partition (no-op in that case).
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "Entering game loop (~60 Hz)");

    while (1) {
        pacman_game_loop();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
