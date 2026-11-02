#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_config.h"
#ifndef SENSOR_DEMO_SERIAL
#include "sht40.h"
#endif
#include "zb_sensor.h"

static const char *TAG = "sensor_main";

static void button_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool pressed = false;
    TickType_t t0 = 0;
    for (;;) {
        int level = gpio_get_level(BUTTON_GPIO);
        if (level == 0 && !pressed) {
            pressed = true;
            t0 = xTaskGetTickCount();
        } else if (level == 1 && pressed) {
            uint32_t held = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            pressed = false;
            if (held >= BUTTON_RESET_MS) {
                ESP_LOGW(TAG, "Factory reset Zigbee state");
                vTaskDelay(pdMS_TO_TICKS(200));
                zb_sensor_factory_reset();
            } else {
                ESP_LOGI(TAG, "Hold BOOT %.1fs to factory-reset and rejoin",
                         BUTTON_RESET_MS / 1000.0f);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#ifndef SENSOR_DEMO_SERIAL
    if (sht40_init() != ESP_OK) {
        ESP_LOGE(TAG, "SHT40 init failed (SDA %d SCL %d)",
                 SHT40_SDA_GPIO, SHT40_SCL_GPIO);
    }
#endif
    xTaskCreate(button_task, "btn", 2048, NULL, 2, NULL);
    zb_sensor_start();
#ifdef SENSOR_DEMO_SERIAL
    ESP_LOGW(TAG, "ESP32-H2 DEMO sensor: enter temperature and humidity in the serial monitor");
    ESP_LOGI(TAG, "Input format: set <temperature_C> <humidity_pct> [battery_pct]  (example: set 24.5 55.0 87)");
#else
    ESP_LOGI(TAG, "SHT40 Zigbee sensor on ESP32-C6-MINI-1 SDA %d SCL %d. Open hub pairing to join.",
             SHT40_SDA_GPIO, SHT40_SCL_GPIO);
#endif
}
