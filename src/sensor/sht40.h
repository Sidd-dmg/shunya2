#ifndef SHT40_H
#define SHT40_H

#include "esp_err.h"

/* Sensirion SHT40 on I2C address 0x44. Pins are SHT40_SDA_GPIO / SHT40_SCL_GPIO. */
esp_err_t sht40_init(void);
esp_err_t sht40_read(float *temperature_c, float *humidity_pct);

#endif
