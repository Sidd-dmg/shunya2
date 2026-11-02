#include "sht40.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_config.h"

/* High-precision single shot. The sensor needs about 8.3 ms before the result
 * can be read. The hardware I2C driver reports both a missing ACK and a clock
 * stretch as ESP_ERR_INVALID_STATE, so this short bus is driven directly. */
#define SHT40_CMD_MEASURE_HIGH 0xFD
#define SHT40_MEASURE_WAIT_MS  10
#define SHT40_HALF_PERIOD_US   20

static const char *TAG = "sht40";

static gpio_num_t s_sda = (gpio_num_t)SHT40_SDA_GPIO;
static gpio_num_t s_scl = (gpio_num_t)SHT40_SCL_GPIO;
static bool s_ready;
static bool s_present;

static uint8_t sht40_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void line_release(gpio_num_t pin)
{
    gpio_set_level(pin, 1);
    esp_rom_delay_us(SHT40_HALF_PERIOD_US);
}

static void line_low(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
    esp_rom_delay_us(SHT40_HALF_PERIOD_US);
}

static bool scl_release(void)
{
    gpio_set_level(s_scl, 1);
    for (int i = 0; i < 1500; ++i) {
        if (gpio_get_level(s_scl)) {
            esp_rom_delay_us(SHT40_HALF_PERIOD_US);
            return true;
        }
        esp_rom_delay_us(10);
    }
    return false;
}

static void i2c_start(void)
{
    line_release(s_sda);
    scl_release();
    line_low(s_sda);
    line_low(s_scl);
}

static void i2c_stop(void)
{
    line_low(s_sda);
    scl_release();
    line_release(s_sda);
}

static bool write_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; --bit) {
        if (value & (1u << bit)) line_release(s_sda);
        else line_low(s_sda);
        if (!scl_release()) return false;
        line_low(s_scl);
    }
    line_release(s_sda);
    bool ack = false;
    if (scl_release()) ack = gpio_get_level(s_sda) == 0;
    line_low(s_scl);
    return ack;
}

static uint8_t read_byte(bool ack)
{
    uint8_t value = 0;
    line_release(s_sda);
    for (int bit = 7; bit >= 0; --bit) {
        if (!scl_release()) return 0;
        if (gpio_get_level(s_sda)) value |= (uint8_t)(1u << bit);
        line_low(s_scl);
    }
    if (ack) line_low(s_sda);
    else line_release(s_sda);
    scl_release();
    line_low(s_scl);
    line_release(s_sda);
    return value;
}

static void configure_pins(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_sda) | (1ULL << s_scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(s_sda, 1);
    gpio_set_level(s_scl, 1);
}

static bool probe_address(uint8_t address)
{
    i2c_start();
    bool ack = write_byte((uint8_t)((address << 1) | 0));
    i2c_stop();
    esp_rom_delay_us(50);
    return ack;
}

static int scan_bus(void)
{
    int found = 0;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (!probe_address(address)) continue;
        ESP_LOGW(TAG, "I2C device ACK at 0x%02X on SDA GPIO %d SCL GPIO %d",
                 address, s_sda, s_scl);
        ++found;
    }
    return found;
}

esp_err_t sht40_init(void)
{
    if (s_ready && s_present) return ESP_OK;

    configure_pins();
    s_present = probe_address(SHT40_I2C_ADDR);
    if (!s_present) {
        ESP_LOGW(TAG, "no SHT40 ACK at 0x44 on SDA %d SCL %d (levels %d/%d); trying swapped pins",
                 s_sda, s_scl, gpio_get_level(s_sda), gpio_get_level(s_scl));
        gpio_num_t swapped = s_sda;
        s_sda = s_scl;
        s_scl = swapped;
        configure_pins();
        s_present = probe_address(SHT40_I2C_ADDR);
    }

    if (s_present) {
        ESP_LOGI(TAG, "SHT40 found at 0x44, SDA GPIO %d, SCL GPIO %d", s_sda, s_scl);
    } else {
        scan_bus();
        gpio_num_t swapped = s_sda;
        s_sda = s_scl;
        s_scl = swapped;
        configure_pins();
        if (!scan_bus()) {
            ESP_LOGE(TAG, "no I2C device on GPIO 22/23. Check 3.3 V, GND, and 4.7 kOhm pull-ups.");
        }
        s_sda = (gpio_num_t)SHT40_SDA_GPIO;
        s_scl = (gpio_num_t)SHT40_SCL_GPIO;
        configure_pins();
    }

    s_ready = true;
    return s_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sht40_read(float *temperature_c, float *humidity_pct)
{
    if (!temperature_c || !humidity_pct) return ESP_ERR_INVALID_ARG;
    esp_err_t err = sht40_init();
    if (err != ESP_OK) return err;

    i2c_start();
    bool addressed = write_byte((uint8_t)((SHT40_I2C_ADDR << 1) | 0));
    bool commanded = addressed && write_byte(SHT40_CMD_MEASURE_HIGH);
    i2c_stop();
    if (!commanded) {
        ESP_LOGW(TAG, "SHT40 %s NACK", addressed ? "command" : "address");
        /* Do not cache a temporary NACK forever. The next scheduled sample
         * probes both wire orientations again, which also recovers a sensor
         * that powered up after the C6. */
        s_ready = false;
        s_present = false;
        return ESP_ERR_NOT_FOUND;
    }

    vTaskDelay(pdMS_TO_TICKS(SHT40_MEASURE_WAIT_MS));
    i2c_start();
    if (!write_byte((uint8_t)((SHT40_I2C_ADDR << 1) | 1))) {
        i2c_stop();
        ESP_LOGW(TAG, "SHT40 read NACK");
        s_ready = false;
        s_present = false;
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t rx[6];
    for (int i = 0; i < 6; ++i) rx[i] = read_byte(i < 5);
    i2c_stop();

    if (sht40_crc(rx, 2) != rx[2] || sht40_crc(rx + 3, 2) != rx[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_t = ((uint16_t)rx[0] << 8) | rx[1];
    const uint16_t raw_h = ((uint16_t)rx[3] << 8) | rx[4];
    *temperature_c = -45.0f + (175.0f * (float)raw_t / 65535.0f);
    float humidity = -6.0f + (125.0f * (float)raw_h / 65535.0f);
    if (humidity < 0.0f) humidity = 0.0f;
    if (humidity > 100.0f) humidity = 100.0f;
    *humidity_pct = humidity;
    return ESP_OK;
}
