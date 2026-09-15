#include "dht.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dht";

#define DHT_TIMEOUT_US 100

static int wait_level(int gpio, int level, int timeout_us) {
    int t = 0;
    while (gpio_get_level(gpio) == level) {
        if (t >= timeout_us) {
            return -1;
        }
        esp_rom_delay_us(1);
        t++;
    }
    return t;
}

esp_err_t dht_init(int gpio) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t dht_read(int gpio, int model, float *temperature_c, float *humidity_pct) {
    uint8_t data[5] = {0};

    gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(gpio, 0);
    if (model == 11) {
        esp_rom_delay_us(20000);
    } else {
        esp_rom_delay_us(2000);
    }
    gpio_set_level(gpio, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);

    portDISABLE_INTERRUPTS();
    if (wait_level(gpio, 1, DHT_TIMEOUT_US) < 0) {
        portENABLE_INTERRUPTS();
        return ESP_ERR_TIMEOUT;
    }
    if (wait_level(gpio, 0, DHT_TIMEOUT_US) < 0) {
        portENABLE_INTERRUPTS();
        return ESP_ERR_TIMEOUT;
    }
    if (wait_level(gpio, 1, DHT_TIMEOUT_US) < 0) {
        portENABLE_INTERRUPTS();
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < 40; i++) {
        if (wait_level(gpio, 0, DHT_TIMEOUT_US) < 0) {
            portENABLE_INTERRUPTS();
            return ESP_ERR_TIMEOUT;
        }
        int high = wait_level(gpio, 1, DHT_TIMEOUT_US);
        if (high < 0) {
            portENABLE_INTERRUPTS();
            return ESP_ERR_TIMEOUT;
        }
        data[i / 8] <<= 1;
        if (high > 40) {
            data[i / 8] |= 1;
        }
    }
    portENABLE_INTERRUPTS();

    uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) {
        ESP_LOGW(TAG, "checksum fail %02x %02x %02x %02x %02x",
                 data[0], data[1], data[2], data[3], data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    float hum;
    float temp;
    if (model == 11) {
        hum = (float)data[0];
        temp = (float)data[2];
    } else {
        hum = ((data[0] << 8) | data[1]) * 0.1f;
        int16_t raw_t = (int16_t)((data[2] << 8) | data[3]);
        if (raw_t & 0x8000) {
            raw_t = -(raw_t & 0x7FFF);
        }
        temp = raw_t * 0.1f;
    }

    if (hum < 0 || hum > 100 || temp < -40 || temp > 80) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (temperature_c) {
        *temperature_c = temp;
    }
    if (humidity_pct) {
        *humidity_pct = hum;
    }
    return ESP_OK;
}
