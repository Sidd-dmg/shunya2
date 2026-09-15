#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "slots.h"
#include "hub_uart.h"
#include "zigbee_coord.h"
#include "hub_config.h"

static const char *TAG = "hub_main";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    slots_init();
    hub_uart_init();
    xTaskCreate(hub_uart_task, "hub_uart", 4096, NULL, 4, NULL);
    zigbee_coord_start();
    ESP_LOGI(TAG, "ESP32-C6 Zigbee hub started, UART GPIO RX%d TX%d",
             HUB_UART_RX_GPIO, HUB_UART_TX_GPIO);
}
